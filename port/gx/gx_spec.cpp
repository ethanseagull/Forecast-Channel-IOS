// Specialized TEV pixel shaders at run time (gx_spec.h): the cache, the
// compile worker, the main thread's object budget and the remembered keys.
//
// Threads. The worker generates the source (gx_tevgen.cpp) and, on Windows,
// compiles it to DXIL with the SDK's dxcompiler.dll. SDL_CreateGPUShader and
// SDL_CreateGPUGraphicsPipeline carry no thread-safety promise in SDL_gpu.h,
// so by default the SDL objects are created on the main thread, a few
// milliseconds' worth a frame. On Apple the expensive step (Metal compiling
// the MSL) happens inside SDL_CreateGPUShader, so there it runs on the
// worker: SDL's Metal backend only calls MTLDevice (thread-safe) and fills
// its own new object there. FORECAST_SPECIALIZED=main / worker overrides.
//
// Memory: the worker is a host thread (port_mark_host_thread), so its
// allocations come from the C heap and are freed by address wherever they
// are deleted. It reads keys from a fixed array and never frees anything the
// game thread allocated.
#include "gx_spec.h"

#include <stdio.h>
#include <string.h>

#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "gx_internal.h"
#include "gx_tevgen.h"
#ifdef _WIN32
#include "gx_dxc.h"
#endif
#include <port_log.h>
#include <port_paths.h>

namespace {

enum Mode { MODE_OFF, MODE_ASYNC, MODE_SYNC };

const u32 kMaxShaders = 1024;
// Main-thread time a frame for creating SDL shaders and pipelines.
const u64 kBudgetNS = 3000000;

SDL_GPUDevice* sDev;
Mode sMode = MODE_OFF;
bool sCreateOnWorker;
SDL_GPUShaderFormat sFormat;
const char* sEntry;
const char* sDumpDir;

enum State : u8 { ST_QUEUED, ST_COMPILED, ST_READY, ST_FAILED };

// A finished compile, made by whichever thread compiled it.
struct Result {
    bool ok;
    std::vector<u8> code;  // DXIL or MSL text (+ NUL)
    SDL_GPUShader* shader; // when created on the worker
    u64 genNS, compileNS, createNS;
    u32 sourceBytes;
    std::string error;
};

// Shader id n (1-based) is slot n - 1. Keys are written by the main thread
// before the worker is told about them (sQueued), then never change.
TevKey sKeys[kMaxShaders];
u64 sHashes[kMaxShaders];
State sState[kMaxShaders];
SDL_GPUShader* sShaders[kMaxShaders];
Result* sResults[kMaxShaders]; // worker -> main, under sLock
u32 sCount;                     // ids in use (main thread)
u32 sTaken;                     // results the main thread has taken (in order)
std::unordered_map<u64, u32> sByHash; // hash -> id; collisions chain through sNext
u32 sNext[kMaxShaders];

SDL_Mutex* sLock;
SDL_Condition* sWake;
u32 sQueued; // ids the worker may compile (under sLock)
u32 sDone;   // ids it has finished (under sLock)

// The last draw's registers and shader, to skip the key for repeated state.
TevGenRegs sLastRegs;
u32 sLastId;
bool sLastValid;

u64 sBudgetUsed;
u32 sReadyCount, sFailedCount;
u64 sSpecDraws, sUberDraws;
u64 sFrameUsed[kMaxShaders / 64]; // ids drawn this frame
u32 sFrameSpec, sFrameUber;

// Remembered pipelines (from the data folder) to create once their shader
// is ready.
struct WantPipe {
    u32 id, bits;
};
std::vector<WantPipe> sWantPipes;
FILE* sKeyFile;

const u32 kFileMagic = 0x4B564554; // "TEVK"
const u32 kFileVersion = 1;
const u32 kNoPipe = 0xFFFFFFFFu;

Result* compile(const TevKey& key, u32 id) {
    Result* r = new Result();
    r->ok = false;
    r->shader = NULL;
    Uint64 t0 = SDL_GetTicksNS();
#ifdef _WIN32
    std::string src = tevgen_source(key, TEVGEN_HLSL);
#else
    std::string src = tevgen_source(key, TEVGEN_MSL);
#endif
    Uint64 t1 = SDL_GetTicksNS();
    r->genNS = t1 - t0;
    r->sourceBytes = (u32)src.size();
    if (sDumpDir != NULL) {
        char path[512];
        static const char* const ext[2] = {"hlsl", "metal"};
        for (int d = 0; d < 2; d++) {
            SDL_snprintf(path, sizeof(path), "%s/tev_%03u_%016llx.%s", sDumpDir, (unsigned)id,
                         (unsigned long long)tevgen_hash(key), ext[d]);
            std::string text = tevgen_source(key, d == 0 ? TEVGEN_HLSL : TEVGEN_MSL);
            FILE* f = fopen(path, "wb");
            if (f != NULL) {
                fwrite(text.data(), 1, text.size(), f);
                fclose(f);
            }
        }
    }
#ifdef _WIN32
    std::string err;
    r->ok = gx_dxc_compile(src, &r->code, &err);
    if (!r->ok) {
        r->error = err;
    }
#else
    r->code.assign(src.begin(), src.end());
    r->code.push_back(0);
    r->ok = true;
#endif
    Uint64 t2 = SDL_GetTicksNS();
    r->compileNS = t2 - t1;
    r->createNS = 0;
    return r;
}

SDL_GPUShader* create_shader(const Result* r) {
    SDL_GPUShaderCreateInfo si = {};
    si.code = r->code.data();
    // MSL: the text without its terminator (as gx_sdl.cpp passes the
    // ubershader's).
    si.code_size = sFormat == SDL_GPU_SHADERFORMAT_MSL ? r->code.size() - 1 : r->code.size();
    si.entrypoint = sEntry;
    si.format = sFormat;
    si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    si.num_samplers = 8;
    si.num_uniform_buffers = 1;
    return SDL_CreateGPUShader(sDev, &si);
}

int SDLCALL worker(void* unused) {
    (void)unused;
    port_mark_host_thread();
    for (;;) {
        SDL_LockMutex(sLock);
        while (sDone == sQueued) {
            SDL_WaitCondition(sWake, sLock);
        }
        u32 slot = sDone;
        SDL_UnlockMutex(sLock);

        Result* r = compile(sKeys[slot], slot + 1);
        if (r->ok && sCreateOnWorker) {
            Uint64 t = SDL_GetTicksNS();
            r->shader = create_shader(r);
            r->createNS = SDL_GetTicksNS() - t;
            if (r->shader == NULL) {
                r->ok = false;
                r->error = SDL_GetError();
            }
            r->code.clear();
            r->code.shrink_to_fit();
        }

        SDL_LockMutex(sLock);
        sResults[slot] = r;
        sDone = slot + 1;
        SDL_UnlockMutex(sLock);
    }
    return 0;
}

// Main thread: makes the shader object of a compiled result (unless the
// worker did) and logs it.
void finish(u32 slot, Result* r) {
    if (r->ok && r->shader == NULL) {
        Uint64 t = SDL_GetTicksNS();
        r->shader = create_shader(r);
        r->createNS = SDL_GetTicksNS() - t;
        if (sMode != MODE_SYNC) {
            sBudgetUsed += r->createNS;
        }
        if (r->shader == NULL) {
            r->ok = false;
            r->error = SDL_GetError();
        }
    }
    if (r->ok) {
        sShaders[slot] = r->shader;
        sState[slot] = ST_READY;
        sReadyCount++;
        port_log("gx: tev shader %u: %s; %u bytes, generated %.2f ms, compiled %.1f ms, created %.1f ms\n",
                 (unsigned)(slot + 1), tevgen_describe(sKeys[slot]).c_str(), (unsigned)r->sourceBytes,
                 r->genNS / 1e6, r->compileNS / 1e6, r->createNS / 1e6);
    } else {
        sState[slot] = ST_FAILED;
        sFailedCount++;
        port_log("gx: tev shader %u failed (ubershader instead): %s\n%s\n", (unsigned)(slot + 1),
                 tevgen_describe(sKeys[slot]).c_str(), r->error.c_str());
    }
    delete r;
}

void remember(u32 id, u32 pipeBits) {
    if (sKeyFile == NULL) {
        return;
    }
    u32 rec[1 + sizeof(TevKey) / 4];
    rec[0] = pipeBits;
    memcpy(&rec[1], &sKeys[id - 1], sizeof(TevKey));
    fwrite(rec, sizeof(rec), 1, sKeyFile);
    fflush(sKeyFile);
}

// Adds a key: returns its id, or 0 when the cache is full.
u32 add_key(const TevKey& key, u64 hash, bool persist) {
    if (sCount >= kMaxShaders) {
        static bool sWarned;
        if (!sWarned) {
            sWarned = true;
            port_log("gx: %u tev shaders: no more specialized ones\n", (unsigned)kMaxShaders);
        }
        return 0;
    }
    u32 slot = sCount++;
    sKeys[slot] = key;
    sHashes[slot] = hash;
    sState[slot] = ST_QUEUED;
    sShaders[slot] = NULL;
    auto it = sByHash.find(hash);
    sNext[slot] = it != sByHash.end() ? it->second : 0;
    sByHash[hash] = slot + 1;
    if (persist) {
        remember(slot + 1, kNoPipe);
    }
    if (sDumpDir != NULL) {
        char path[512];
        SDL_snprintf(path, sizeof(path), "%s/keys.txt", sDumpDir);
        FILE* f = fopen(path, "ab");
        if (f != NULL) {
            fprintf(f, "%s\n", tevgen_key_to_text(key).c_str());
            fclose(f);
        }
    }
    if (sMode == MODE_SYNC) {
        // Compile and create now (testing: every draw specialized).
        sState[slot] = ST_COMPILED;
        finish(slot, compile(key, slot + 1));
        return slot + 1;
    }
    SDL_LockMutex(sLock);
    sQueued = slot + 1;
    SDL_SignalCondition(sWake);
    SDL_UnlockMutex(sLock);
    return slot + 1;
}

u32 find_key(const TevKey& key, u64 hash) {
    auto it = sByHash.find(hash);
    for (u32 id = it != sByHash.end() ? it->second : 0; id != 0; id = sNext[id - 1]) {
        if (memcmp(&sKeys[id - 1], &key, sizeof(key)) == 0) {
            return id;
        }
    }
    return 0;
}

// The data folder's list of keys and pipelines seen in earlier runs: queued
// for compiling at start-up.
void load_keys(void) {
    std::string path = std::string(port_data_dir()) + "tev_shaders.bin";
    FILE* f = fopen(path.c_str(), "rb");
    u32 header[2] = {0, 0};
    u32 loaded = 0;
    if (f != NULL) {
        if (fread(header, sizeof(header), 1, f) == 1 && header[0] == kFileMagic && header[1] == kFileVersion) {
            u32 rec[1 + sizeof(TevKey) / 4];
            while (fread(rec, sizeof(rec), 1, f) == 1) {
                TevKey key;
                memcpy(&key, &rec[1], sizeof(key));
                if (key.numStages < 1 || key.numStages > 16 || key.numInd > 4) {
                    break;
                }
                u64 hash = tevgen_hash(key);
                u32 id = find_key(key, hash);
                if (id == 0) {
                    id = add_key(key, hash, false);
                    loaded += id != 0;
                }
                if (id != 0 && rec[0] != kNoPipe) {
                    bool dup = false;
                    for (const WantPipe& w : sWantPipes) {
                        dup |= w.id == id && w.bits == rec[0];
                    }
                    if (!dup) {
                        sWantPipes.push_back({id, rec[0]});
                    }
                }
            }
        }
        fclose(f);
    }
    bool valid = header[0] == kFileMagic && header[1] == kFileVersion;
    // Rewrite it compacted (one record a shader, one a pipeline).
    sKeyFile = fopen(path.c_str(), "wb");
    if (sKeyFile != NULL) {
        u32 h[2] = {kFileMagic, kFileVersion};
        fwrite(h, sizeof(h), 1, sKeyFile);
        for (u32 id = 1; id <= sCount; id++) {
            remember(id, kNoPipe);
        }
        for (const WantPipe& w : sWantPipes) {
            remember(w.id, w.bits);
        }
    }
    if (loaded != 0 || (f != NULL && !valid)) {
        port_log("gx: %u tev shaders and %u pipelines remembered from earlier runs%s\n", (unsigned)loaded,
                 (unsigned)sWantPipes.size(), valid ? "" : " (old file format: dropped)");
    }
}

} // namespace

void gx_spec_init(SDL_GPUDevice* dev) {
    sDev = dev;
    const char* env = SDL_getenv("FORECAST_SPECIALIZED");
    sMode = MODE_ASYNC;
    if (env != NULL && SDL_strcmp(env, "0") == 0) {
        sMode = MODE_OFF;
    } else if (env != NULL && SDL_strcmp(env, "sync") == 0) {
        sMode = MODE_SYNC;
    }
#ifdef __APPLE__
    sCreateOnWorker = true;
    sFormat = SDL_GPU_SHADERFORMAT_MSL;
    sEntry = "main0";
#else
    sCreateOnWorker = false;
    sFormat = SDL_GPU_SHADERFORMAT_DXIL;
    sEntry = "main";
#endif
    if (env != NULL && SDL_strcmp(env, "worker") == 0) {
        sCreateOnWorker = true;
    } else if (env != NULL && SDL_strcmp(env, "main") == 0) {
        sCreateOnWorker = false;
    }
    if (sMode == MODE_OFF) {
        port_log("gx: specialized TEV shaders off (FORECAST_SPECIALIZED=0)\n");
        return;
    }
#if defined(_WIN32)
    std::string where;
    if (!gx_dxc_load(&where)) {
        port_log("gx: dxcompiler.dll / dxil.dll not found: ubershader only\n");
        sMode = MODE_OFF;
        return;
    }
    port_log("gx: specialized TEV shaders: DXC from %s\n", where.c_str());
#elif defined(__APPLE__)
    port_log("gx: specialized TEV shaders: MSL\n");
#else
    port_log("gx: no shader compiler on this platform: ubershader only\n");
    sMode = MODE_OFF;
    return;
#endif
    if (sMode == MODE_SYNC) {
        port_log("gx: specialized TEV shaders compile synchronously (FORECAST_SPECIALIZED=sync)\n");
    } else {
        port_log("gx: SDL shader objects created on the %s thread\n", sCreateOnWorker ? "worker" : "main");
    }
    sDumpDir = SDL_getenv("FORECAST_SPECIALIZED_DUMP");
    sLock = SDL_CreateMutex();
    sWake = SDL_CreateCondition();
    if (sMode == MODE_ASYNC) {
        SDL_Thread* t = SDL_CreateThread(worker, "tevshaders", NULL);
        if (t == NULL) {
            port_log("gx: shader worker thread failed (%s): ubershader only\n", SDL_GetError());
            sMode = MODE_OFF;
            return;
        }
        SDL_DetachThread(t);
    }
    load_keys();
}

bool gx_spec_enabled(void) {
    return sMode != MODE_OFF;
}

u32 gx_spec_lookup(const GXTevUniforms& u, SDL_GPUShader** shader) {
    *shader = NULL;
    if (sMode == MODE_OFF) {
        return 0;
    }
    TevGenRegs r;
    memcpy(r.colorEnv, u.colorEnv, sizeof(r.colorEnv));
    memcpy(r.alphaEnv, u.alphaEnv, sizeof(r.alphaEnv));
    memcpy(r.tevOrder, u.tevOrder, sizeof(r.tevOrder));
    memcpy(r.ksel, u.ksel, sizeof(r.ksel));
    memcpy(r.tevInd, u.tevInd, sizeof(r.tevInd));
    r.genMode = u.genMode;
    r.alphaCompare = u.alphaCompare;
    r.texBound = u.texBound;
    r.indRef = u.indRef;
    u32 id;
    if (sLastValid && memcmp(&r, &sLastRegs, sizeof(r)) == 0) {
        id = sLastId;
    } else {
        TevKey key;
        tevgen_make_key(r, &key);
        u64 hash = tevgen_hash(key);
        id = find_key(key, hash);
        if (id == 0) {
            id = add_key(key, hash, true);
        }
        sLastRegs = r;
        sLastId = id;
        sLastValid = true;
    }
    if (id != 0 && sState[id - 1] == ST_READY) {
        *shader = sShaders[id - 1];
    }
    return id;
}

bool gx_spec_budget_left(void) {
    return sMode == MODE_SYNC || sBudgetUsed < kBudgetNS;
}

void gx_spec_budget_charge(u64 ns) {
    sBudgetUsed += ns;
}

void gx_spec_pipeline_created(u32 id, u32 pipeBits) {
    if (id != 0) {
        remember(id, pipeBits);
    }
}

void gx_spec_count_draw(u32 id, bool specialized) {
    if (specialized) {
        sSpecDraws++;
        sFrameSpec++;
    } else {
        sUberDraws++;
        sFrameUber++;
    }
    if (id != 0) {
        sFrameUsed[(id - 1) / 64] |= 1ull << ((id - 1) % 64);
    }
}

void gx_spec_frame_end(u32 frame, GXSpecMakePipe make_pipe, bool logFrame) {
    sBudgetUsed = 0;
    if (sMode == MODE_OFF) {
        return;
    }
    if (logFrame) {
        std::string ids;
        u32 n = 0;
        for (u32 i = 0; i < kMaxShaders; i++) {
            if ((sFrameUsed[i / 64] >> (i % 64)) & 1u) {
                char buf[16];
                SDL_snprintf(buf, sizeof(buf), " %u", (unsigned)(i + 1));
                ids += buf;
                n++;
            }
        }
        port_log("gx: frame %lu used %u tev shaders (%s); %u draws specialized, %u ubershader\n",
                 (unsigned long)frame, (unsigned)n, ids.c_str(), (unsigned)sFrameSpec, (unsigned)sFrameUber);
    }
    memset(sFrameUsed, 0, sizeof(sFrameUsed));
    sFrameSpec = sFrameUber = 0;
    if (sMode == MODE_ASYNC) {
        SDL_LockMutex(sLock);
        u32 done = sDone;
        SDL_UnlockMutex(sLock);
        while (sTaken < done && gx_spec_budget_left()) {
            SDL_LockMutex(sLock);
            Result* r = sResults[sTaken];
            sResults[sTaken] = NULL;
            SDL_UnlockMutex(sLock);
            finish(sTaken, r);
            sTaken++;
        }
    }
    // Pipelines remembered from earlier runs, once their shader is ready.
    size_t keep = 0;
    for (size_t i = 0; i < sWantPipes.size(); i++) {
        WantPipe w = sWantPipes[i];
        State st = sState[w.id - 1];
        if (st == ST_FAILED) {
            continue;
        }
        if (st == ST_READY && gx_spec_budget_left() && make_pipe(w.id, sShaders[w.id - 1], w.bits)) {
            continue;
        }
        sWantPipes[keep++] = w;
    }
    sWantPipes.resize(keep);

    if (frame % 600 == 0) {
        u64 total = sSpecDraws + sUberDraws;
        port_log("gx: tev shaders: %u ready, %u compiling, %u failed; last 600 frames: %llu draws specialized, "
                 "%llu ubershader (%.1f%%)\n",
                 (unsigned)sReadyCount, (unsigned)(sCount - sReadyCount - sFailedCount), (unsigned)sFailedCount,
                 (unsigned long long)sSpecDraws, (unsigned long long)sUberDraws,
                 total != 0 ? 100.0 * (double)sSpecDraws / (double)total : 0.0);
        sSpecDraws = sUberDraws = 0;
    }
}
