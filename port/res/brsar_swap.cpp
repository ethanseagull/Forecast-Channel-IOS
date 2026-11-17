// In-place big-endian -> native conversion of NW4R sound archives (BRSAR) and
// the files inside them that nw4r::snd reads in place: sequences (RSEQ),
// banks (RBNK) and wave sound data (RWSD). Done once, when an archive is handed
// to MemorySoundArchive::Setup; files copied out of the archive later (group
// and file loads) are copies of converted data. The byte-order mark in each
// file header tells whether that file is still big-endian, so converting twice
// is harmless.
//
// Converted (the structures in include/nw4r/snd, as src/nw4r/snd reads them):
//   - BRSAR: file header (signature included: snd compares it as a u32), the
//     SYMB string table and the four label trees, the INFO block (sound, bank,
//     player, file and group tables, 3D parameters, seq / wave sound info,
//     file positions, group items, player counts) and the FILE block header.
//   - RSEQ: header, DATA block header and its base offset, the LABL block.
//   - RBNK: header, instrument table with every range / index sub-table and
//     instrument parameter it reaches, wave info table with the wave infos.
//   - RWSD (v1.1+): header, wave sound table (info, track and note tables),
//     wave info offset table and wave infos.
//   - Wave infos (RBNK / RWSD): sample rate, loop start / end, offsets, the
//     channel info table and each channel's info, and the ADPCM info (coefs,
//     gain, initial predictor/scale and history, loop predictor/scale and
//     history). nw4r copies the ADPCM info into WaveData and AxVoice hands it
//     to AXSetVoiceAdpcm / AXSetVoiceAdpcmLoop, so it is CPU data.
//
// Left big-endian on purpose:
//   - Wave sample data (the group wave data blocks: ADPCM frames, PCM16 /
//     PCM8 samples). It is what the DSP read through the voice's addresses;
//     the AX emulation reads it as big-endian, like the hardware.
//   - Sequence bytecode (the RSEQ DATA block after its header): MmlParser
//     reads it a byte at a time and assembles 16/24-bit values by hand
//     (Read16 / Read24 / ReadVar), so it is byte-order independent.
//   - Strings (labels, external file paths) and u8 fields.
//
// Not handled (none in the channel's archives; logged if met): RSTM streams
// (read from external files through file streams, not in place), RWSD v1.0
// (its wave table has no count), REFTYPE_ADDRESS data references.
#include <nw4r/snd.h>

#include <cstddef>
#include <cstring>
#include <unordered_set>

#include <port_log.h>
#include <port_res.h>

namespace nw4r {
namespace snd {
namespace {

using ::port::res::Swap16;
using ::port::res::Swap16n;
using ::port::res::Swap32;
using ::port::res::Swap32n;

namespace SAF = detail::SoundArchiveFile;
typedef detail::Util::DataRef<void> AnyRef;

// The file layouts the converter relies on (and snd reads in place).
static_assert(sizeof(ut::BinaryFileHeader) == 0x10, "BinaryFileHeader");
static_assert(sizeof(ut::BinaryBlockHeader) == 0x8, "BinaryBlockHeader");
static_assert(sizeof(AnyRef) == 0x8, "DataRef");
static_assert(sizeof(SAF::Header) == 0x28, "SoundArchiveFile::Header");
static_assert(sizeof(SAF::StringTreeNode) == 0x14, "StringTreeNode");
static_assert(sizeof(SAF::StringChunk) == 0x14, "StringChunk");
static_assert(sizeof(SAF::SeqSoundInfo) == 0x10, "SeqSoundInfo");
static_assert(sizeof(SAF::WaveSoundInfo) == 0xC, "WaveSoundInfo");
static_assert(offsetof(SAF::SoundCommonInfo, param3dRef) == 0xC, "SoundCommonInfo");
static_assert(offsetof(SAF::SoundCommonInfo, soundInfoRef) == 0x18, "SoundCommonInfo");
static_assert(offsetof(SAF::SoundCommonInfo, userParam) == 0x20, "SoundCommonInfo");
static_assert(sizeof(SAF::BankInfo) == 0x8, "BankInfo");
static_assert(offsetof(SAF::PlayerInfo, heapSize) == 0x8, "PlayerInfo");
static_assert(offsetof(SAF::FileInfo, filePosTableRef) == 0x14, "FileInfo");
static_assert(sizeof(SoundArchive::FilePos) == 0x8, "FilePos");
static_assert(offsetof(SAF::GroupInfo, itemTableRef) == 0x20, "GroupInfo");
static_assert(sizeof(SAF::GroupItemInfo) == 0x14, "GroupItemInfo");
static_assert(sizeof(SAF::SoundArchivePlayerInfo) == 0xE, "SoundArchivePlayerInfo");
static_assert(sizeof(SAF::Info) == 0x30, "Info");
static_assert(sizeof(detail::SeqFile::Header) == 0x20, "SeqFile::Header");
static_assert(sizeof(detail::SeqFile::DataBlock) == 0xC, "SeqFile::DataBlock");
static_assert(sizeof(detail::BankFile::Header) == 0x20, "BankFile::Header");
static_assert(offsetof(detail::BankFile::InstParam, tune) == 0x10, "InstParam");
static_assert(offsetof(detail::BankFile::IndexTable, ref) == 0x4, "IndexTable");
static_assert(offsetof(detail::BankFile::DataBlock, instTable) == 0x8, "BankFile::DataBlock");
static_assert(offsetof(detail::BankFile::WaveBlock, waveInfoTable) == 0x8, "BankFile::WaveBlock");
static_assert(sizeof(detail::WsdFile::Header) == 0x20, "WsdFile::Header");
static_assert(sizeof(detail::WsdFile::WsdInfo) == 0x20, "WsdInfo");
static_assert(sizeof(detail::WsdFile::NoteInfo) == 0x30, "NoteInfo");
static_assert(sizeof(detail::WsdFile::Wsd) == 0x18, "Wsd");
static_assert(offsetof(detail::WsdFile::DataBlock, refWsd) == 0xC, "WsdFile::DataBlock");
static_assert(offsetof(detail::WsdFile::WaveBlock, offsetTable) == 0xC, "WsdFile::WaveBlock");
static_assert(sizeof(detail::WaveFile::WaveInfo) == 0x1C, "WaveInfo");
static_assert(sizeof(detail::WaveFile::WaveChannelInfo) == 0x1C, "WaveChannelInfo");
static_assert(sizeof(detail::AdpcmInfo) == 0x30, "AdpcmInfo");

// Raw (big-endian) signatures, as the bytes appear in the file.
inline bool HasMagic(const void* p, const char* m) { return std::memcmp(p, m, 4) == 0; }

inline u8* At(const void* base, u32 offset) {
    return const_cast<u8*>(static_cast<const u8*>(base)) + offset;
}

class Converter {
public:
    Converter(u8* begin, u32 size) : mBegin(begin), mEnd(begin + size) {}

    void SwapArchive();
    bool SwapSubFile(u8* p, u32 size);

    int mSounds = 0, mSeqSounds = 0, mWaveSounds = 0, mStrmSounds = 0;
    int mSeqFiles = 0, mBankFiles = 0, mWsdFiles = 0, mOtherFiles = 0;
    int mInsts = 0, mWaves = 0, mAdpcmWaves = 0, mPcm16Waves = 0, mPcm8Waves = 0;
    int mLoopWaves = 0, mMaxChannels = 0;
    u32 mMinRate = 0xFFFFFFFF, mMaxRate = 0;
    bool mOk = true;

private:
    bool First(const void* p) { return mDone.insert(p).second; }

    bool In(const void* p, u32 size) const {
        const u8* b = static_cast<const u8*>(p);
        return b >= mBegin && b <= mEnd && size <= static_cast<size_t>(mEnd - b);
    }

    void Bad(const char* what, const void* p) {
        if (mOk) {
            port_log("snd: brsar %s at +%lx is out of range or unsupported; archive left partly converted\n",
                     what, static_cast<unsigned long>(static_cast<const u8*>(p) - mBegin));
        }
        mOk = false;
    }

    // Swaps a DataRef in place and resolves it against `base`. NULL for a null
    // reference (address 0) and for anything this port cannot follow.
    u8* Ref(void* r, const void* base, u32 targetSize) {
        AnyRef* ref = static_cast<AnyRef*>(r);
        Swap16(&ref->reserved);
        Swap32(&ref->value);
        if (ref->refType == detail::Util::REFTYPE_OFFSET) {
            u8* p = At(base, ref->value);
            if (!In(p, targetSize)) {
                Bad("data reference", r);
                return NULL;
            }
            return p;
        }
        if (ref->refType == detail::Util::REFTYPE_ADDRESS && ref->value == 0) {
            return NULL;
        }
        Bad("address data reference", r);
        return NULL;
    }

    // Util::Table<T>: count, then count items of `itemSize` bytes. Returns the
    // count (0 if the table is out of range); swaps the count once.
    u32 Table(void* t, u32 itemSize) {
        if (!In(t, 4)) {
            Bad("table", t);
            return 0;
        }
        if (First(t)) {
            Swap32(t);
        }
        u32 n;
        std::memcpy(&n, t, 4);
        if (!In(At(t, 4), n * itemSize)) {
            Bad("table items", t);
            return 0;
        }
        return n;
    }

    void SwapFileHeader(ut::BinaryFileHeader* h) {
        Swap32(&h->signature);
        Swap16(&h->byteOrder);
        Swap16(&h->version);
        Swap32(&h->fileSize);
        Swap16(&h->headerSize);
        Swap16(&h->dataBlocks);
    }

    // A block header inside the current file; false if out of range.
    bool SwapBlockHeader(void* p) {
        if (!In(p, sizeof(ut::BinaryBlockHeader))) {
            Bad("block", p);
            return false;
        }
        ut::BinaryBlockHeader* b = static_cast<ut::BinaryBlockHeader*>(p);
        Swap32(&b->kind);
        Swap32(&b->size);
        return true;
    }

    void SwapSymbols(u8* block);
    void SwapStringTree(u8* tree);
    void SwapInfo(u8* block);
    void SwapSoundInfo(u8* info, const u8* base, int index);
    void SwapFileInfo(u8* info, const u8* base);
    void SwapGroupInfo(u8* info, const u8* base);
    void SwapGroupFiles();

    void SwapSeq(u8* f);
    void SwapBank(u8* f);
    void SwapBankRegion(void* ref, const u8* base, int depth);
    void SwapWsd(u8* f);
    void SwapWaveInfo(u8* w);

    u8* mBegin;
    u8* mEnd;
    const SAF::Info* mInfo = NULL;
    std::unordered_set<const void*> mDone;
};

// ---------------------------------------------------------------------------
// BRSAR
// ---------------------------------------------------------------------------

void Converter::SwapArchive() {
    SAF::Header* h = reinterpret_cast<SAF::Header*>(mBegin);
    Swap32n(&h->symbolDataOffset, 6);
    if (h->fileHeader.version > detail::SoundArchiveFileReader::VERSION ||
        h->fileHeader.version < NW4R_VERSION(1, 1)) {
        // v1.0 sound tables hold the infos directly (impl_GetSoundInfo).
        Bad("archive version", mBegin);
        return;
    }
    if (h->symbolDataOffset != 0) {
        SwapSymbols(At(mBegin, h->symbolDataOffset));
    }
    SwapInfo(At(mBegin, h->infoOffset));
    if (h->fileImageOffset != 0) {
        SwapBlockHeader(At(mBegin, h->fileImageOffset));
    }
    SwapGroupFiles();
}

// SYMB: string table and the sound / player / group / bank label trees, all
// offsets relative to the string chunk (block + 8).
void Converter::SwapSymbols(u8* block) {
    if (!SwapBlockHeader(block) || !In(block, sizeof(SAF::SymbolBlock))) {
        return;
    }
    u8* base = block + offsetof(SAF::SymbolBlock, stringBlock);
    SAF::StringChunk* c = reinterpret_cast<SAF::StringChunk*>(base);
    Swap32n(c, 5);
    if (c->tableOffset != 0) {
        u8* t = At(base, c->tableOffset);
        u32 n = Table(t, 4);
        Swap32n(At(t, 4), n); // string offsets; the strings are chars
    }
    const u32 trees[4] = {c->soundTreeOffset, c->playerTreeOffset, c->groupTreeOffset, c->bankTreeOffset};
    for (int i = 0; i < 4; i++) {
        if (trees[i] != 0) {
            SwapStringTree(At(base, trees[i]));
        }
    }
}

void Converter::SwapStringTree(u8* tree) {
    if (!In(tree, 8) || !First(tree)) {
        return;
    }
    Swap32(tree); // rootIdx
    u32 n = Table(tree + 4, sizeof(SAF::StringTreeNode));
    SAF::StringTreeNode* nodes = reinterpret_cast<SAF::StringTreeNode*>(tree + 8);
    for (u32 i = 0; i < n; i++) {
        Swap16(&nodes[i].flags);
        Swap16(&nodes[i].bit);
        Swap32n(&nodes[i].leftIdx, 4);
    }
}

// INFO: every reference is relative to the Info structure (block + 8).
void Converter::SwapInfo(u8* block) {
    if (!SwapBlockHeader(block) || !In(block, sizeof(SAF::InfoBlock))) {
        return;
    }
    u8* base = block + offsetof(SAF::InfoBlock, info);
    SAF::Info* info = reinterpret_cast<SAF::Info*>(base);
    mInfo = info;

    if (u8* t = Ref(&info->soundTableRef, base, 4)) {
        u32 n = Table(t, sizeof(AnyRef));
        for (u32 i = 0; i < n; i++) {
            if (u8* s = Ref(At(t, 4 + i * sizeof(AnyRef)), base, sizeof(SAF::SoundCommonInfo))) {
                SwapSoundInfo(s, base, i);
            }
        }
    }
    if (u8* t = Ref(&info->bankTableRef, base, 4)) {
        u32 n = Table(t, sizeof(AnyRef));
        for (u32 i = 0; i < n; i++) {
            u8* b = Ref(At(t, 4 + i * sizeof(AnyRef)), base, sizeof(SAF::BankInfo));
            if (b != NULL && First(b)) {
                Swap32n(b, 2); // stringId, fileId
            }
        }
    }
    if (u8* t = Ref(&info->playerTableRef, base, 4)) {
        u32 n = Table(t, sizeof(AnyRef));
        for (u32 i = 0; i < n; i++) {
            u8* p = Ref(At(t, 4 + i * sizeof(AnyRef)), base, sizeof(SAF::PlayerInfo));
            if (p != NULL && First(p)) {
                SAF::PlayerInfo* pi = reinterpret_cast<SAF::PlayerInfo*>(p);
                Swap32(&pi->stringId);
                Swap32(&pi->heapSize);
            }
        }
    }
    if (u8* t = Ref(&info->fileTableRef, base, 4)) {
        u32 n = Table(t, sizeof(AnyRef));
        for (u32 i = 0; i < n; i++) {
            if (u8* f = Ref(At(t, 4 + i * sizeof(AnyRef)), base, sizeof(SAF::FileInfo))) {
                SwapFileInfo(f, base);
            }
        }
    }
    if (u8* t = Ref(&info->groupTableRef, base, 4)) {
        u32 n = Table(t, sizeof(AnyRef));
        for (u32 i = 0; i < n; i++) {
            if (u8* g = Ref(At(t, 4 + i * sizeof(AnyRef)), base, sizeof(SAF::GroupInfo))) {
                SwapGroupInfo(g, base);
            }
        }
    }
    u8* pi = Ref(&info->soundArchivePlayerInfoRef, base, sizeof(SAF::SoundArchivePlayerInfo));
    if (pi != NULL && First(pi)) {
        Swap16n(pi, 7);
    }
}

void Converter::SwapSoundInfo(u8* p, const u8* base, int index) {
    if (!First(p)) {
        return;
    }
    SAF::SoundCommonInfo* s = reinterpret_cast<SAF::SoundCommonInfo*>(p);
    mSounds++;
    Swap32n(&s->stringId, 3); // stringId, fileId, playerId
    if (u8* d = Ref(&s->param3dRef, base, sizeof(SAF::Sound3DParam))) {
        if (First(d)) {
            Swap32(&reinterpret_cast<SAF::Sound3DParam*>(d)->flags);
        }
    }
    Swap32n(s->userParam, 2);
    switch (s->soundType) {
    case SOUND_TYPE_SEQ: {
        mSeqSounds++;
        u8* d = Ref(&s->soundInfoRef, base, sizeof(SAF::SeqSoundInfo));
        if (d != NULL && First(d)) {
            Swap32n(d, 3); // dataOffset, bankId, allocTrack
        }
        break;
    }
    case SOUND_TYPE_WAVE: {
        mWaveSounds++;
        u8* d = Ref(&s->soundInfoRef, base, sizeof(SAF::WaveSoundInfo));
        if (d != NULL && First(d)) {
            Swap32n(d, 2); // subNo, allocTrack
        }
        break;
    }
    case SOUND_TYPE_STRM:
        // StrmSoundInfo is empty in this NW4R: nothing of it is read.
        mStrmSounds++;
        Ref(&s->soundInfoRef, base, 0);
        break;
    default:
        port_log("snd: brsar sound %d has unknown type %d\n", index, s->soundType);
        Ref(&s->soundInfoRef, base, 0);
        break;
    }
}

void Converter::SwapFileInfo(u8* p, const u8* base) {
    if (!First(p)) {
        return;
    }
    SAF::FileInfo* f = reinterpret_cast<SAF::FileInfo*>(p);
    Swap32n(&f->fileSize, 3); // fileSize, waveDataSize, entryNum
    Ref(&f->extFilePathRef, base, 1);
    if (u8* t = Ref(&f->filePosTableRef, base, 4)) {
        u32 n = Table(t, sizeof(AnyRef));
        if (!First(At(t, 4))) {
            return; // table shared with another file: already converted
        }
        for (u32 i = 0; i < n; i++) {
            u8* pos = Ref(At(t, 4 + i * sizeof(AnyRef)), base, sizeof(SoundArchive::FilePos));
            if (pos != NULL && First(pos)) {
                Swap32n(pos, 2); // groupId, index
            }
        }
    }
}

void Converter::SwapGroupInfo(u8* p, const u8* base) {
    if (!First(p)) {
        return;
    }
    SAF::GroupInfo* g = reinterpret_cast<SAF::GroupInfo*>(p);
    Swap32n(&g->stringId, 2); // stringId, entryNum
    Ref(&g->extFilePathRef, base, 1);
    Swap32n(&g->offset, 4); // offset, size, waveDataOffset, waveDataSize
    if (u8* t = Ref(&g->itemTableRef, base, 4)) {
        u32 n = Table(t, sizeof(AnyRef));
        if (!First(At(t, 4))) {
            return;
        }
        for (u32 i = 0; i < n; i++) {
            u8* item = Ref(At(t, 4 + i * sizeof(AnyRef)), base, sizeof(SAF::GroupItemInfo));
            if (item != NULL && First(item)) {
                Swap32n(item, 5);
            }
        }
    }
}

// The files themselves: every group item, at the group's offset plus the
// item's (MemorySoundArchive::detail_GetFileAddress). The group wave data is
// sample data and stays big-endian.
void Converter::SwapGroupFiles() {
    if (mInfo == NULL) {
        return;
    }
    const u8* base = reinterpret_cast<const u8*>(mInfo);
    const SAF::GroupTable* groups = detail::Util::GetDataRefAddress0(mInfo->groupTableRef, base);
    // (Every table and info below was range-checked while it was converted.)
    if (!mOk || groups == NULL) {
        return;
    }
    for (u32 i = 0; i < groups->count; i++) {
        const SAF::GroupInfo* g = detail::Util::GetDataRefAddress0(groups->items[i], base);
        if (g == NULL || detail::Util::GetDataRefAddress0(g->extFilePathRef, base) != NULL) {
            continue; // external group: loaded from a file, not in this buffer
        }
        const SAF::GroupItemTable* items = detail::Util::GetDataRefAddress0(g->itemTableRef, base);
        if (items == NULL) {
            continue;
        }
        for (u32 k = 0; k < items->count; k++) {
            const SAF::GroupItemInfo* item = detail::Util::GetDataRefAddress0(items->items[k], base);
            if (item == NULL) {
                continue;
            }
            u8* f = At(mBegin, g->offset + item->offset);
            if (!In(f, item->size) || item->size < sizeof(ut::BinaryFileHeader)) {
                Bad("group file", f);
                continue;
            }
            Converter sub(f, item->size);
            if (sub.SwapSubFile(f, item->size)) {
                mSeqFiles += sub.mSeqFiles;
                mBankFiles += sub.mBankFiles;
                mWsdFiles += sub.mWsdFiles;
                mOtherFiles += sub.mOtherFiles;
                mInsts += sub.mInsts;
                mWaves += sub.mWaves;
                mAdpcmWaves += sub.mAdpcmWaves;
                mPcm16Waves += sub.mPcm16Waves;
                mPcm8Waves += sub.mPcm8Waves;
                mLoopWaves += sub.mLoopWaves;
                mMaxChannels = sub.mMaxChannels > mMaxChannels ? sub.mMaxChannels : mMaxChannels;
                mMinRate = sub.mMinRate < mMinRate ? sub.mMinRate : mMinRate;
                mMaxRate = sub.mMaxRate > mMaxRate ? sub.mMaxRate : mMaxRate;
                mOk = mOk && sub.mOk;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Files inside the archive
// ---------------------------------------------------------------------------

// Converts one file (RSEQ / RBNK / RWSD) that starts at `f`. False if it was
// already native (byte-order mark) or is not a file snd reads in place.
bool Converter::SwapSubFile(u8* f, u32 size) {
    ut::BinaryFileHeader* h = reinterpret_cast<ut::BinaryFileHeader*>(f);
    if (h->byteOrder != 0xFFFE) {
        return false; // already native
    }
    void (Converter::*fn)(u8*) = NULL;
    if (HasMagic(f, "RSEQ")) {
        fn = &Converter::SwapSeq;
        mSeqFiles++;
    } else if (HasMagic(f, "RBNK")) {
        fn = &Converter::SwapBank;
        mBankFiles++;
    } else if (HasMagic(f, "RWSD")) {
        fn = &Converter::SwapWsd;
        mWsdFiles++;
    } else {
        port_log("snd: brsar file %.4s (%lu bytes) not converted\n", reinterpret_cast<const char*>(f),
                 static_cast<unsigned long>(size));
        mOtherFiles++;
        return true;
    }
    SwapFileHeader(h);
    (this->*fn)(f);
    return true;
}

// RSEQ: the header and the DATA block's base offset; the bytecode after it
// stays as it is. LABL (labels; not read by snd) is converted for
// consistency: count, offsets, and each label's data offset and length.
void Converter::SwapSeq(u8* f) {
    detail::SeqFile::Header* h = reinterpret_cast<detail::SeqFile::Header*>(f);
    Swap32n(&h->dataBlockOffset, 4);
    u8* data = At(f, h->dataBlockOffset);
    if (SwapBlockHeader(data) && In(data, sizeof(detail::SeqFile::DataBlock))) {
        Swap32(&reinterpret_cast<detail::SeqFile::DataBlock*>(data)->baseOffset);
    }
    if (h->labelBlockOffset != 0) {
        u8* labl = At(f, h->labelBlockOffset);
        if (SwapBlockHeader(labl)) {
            u8* base = labl + sizeof(ut::BinaryBlockHeader);
            u32 n = Table(base, 4);
            for (u32 i = 0; i < n; i++) {
                u8* ofs = At(base, 4 + i * 4);
                Swap32(ofs);
                u32 o;
                std::memcpy(&o, ofs, 4);
                u8* label = At(base, o);
                if (In(label, 8) && First(label)) {
                    Swap32n(label, 2); // data offset, name length
                }
            }
        }
    }
}

// RBNK: instrument regions are relative to the instrument table, wave info
// references to the wave info table (BankFileReader).
void Converter::SwapBank(u8* f) {
    detail::BankFile::Header* h = reinterpret_cast<detail::BankFile::Header*>(f);
    Swap32n(&h->dataBlockOffset, 4);

    u8* data = At(f, h->dataBlockOffset);
    if (SwapBlockHeader(data)) {
        u8* table = data + offsetof(detail::BankFile::DataBlock, instTable);
        u32 n = Table(table, sizeof(AnyRef));
        mInsts += n;
        for (u32 i = 0; i < n; i++) {
            SwapBankRegion(At(table, 4 + i * sizeof(AnyRef)), table, 0);
        }
    }

    if (h->waveBlockOffset != 0) {
        u8* wave = At(f, h->waveBlockOffset);
        if (SwapBlockHeader(wave)) {
            u8* table = wave + offsetof(detail::BankFile::WaveBlock, waveInfoTable);
            u32 n = Table(table, sizeof(AnyRef));
            for (u32 i = 0; i < n; i++) {
                if (u8* w = Ref(At(table, 4 + i * sizeof(AnyRef)), table, sizeof(detail::WaveFile::WaveInfo))) {
                    SwapWaveInfo(w);
                }
            }
        }
    }
}

// One instrument region reference (BankFileReader::ReadInstInfo /
// GetReferenceToSubRegion): an instrument parameter, or a key / velocity
// range or index table of further references.
void Converter::SwapBankRegion(void* ref, const u8* base, int depth) {
    const u8 type = static_cast<AnyRef*>(ref)->dataType;
    u8* p = Ref(ref, base, 1);
    if (p == NULL || depth > 4 || !First(p)) {
        return;
    }
    switch (type) {
    case detail::Util::DATATYPE_T1: { // InstParam
        if (!In(p, sizeof(detail::BankFile::InstParam))) {
            Bad("instrument", p);
            return;
        }
        detail::BankFile::InstParam* inst = reinterpret_cast<detail::BankFile::InstParam*>(p);
        Swap32(&inst->waveIndex);
        Swap32(&inst->tune);
        break;
    }
    case detail::Util::DATATYPE_T2: { // RangeTable
        const u32 n = p[0]; // tableSize, then the keys, then the references
        u8* refs = p + ut::RoundUp<u32>(n + 1, 4);
        if (!In(refs, n * sizeof(AnyRef))) {
            Bad("range table", p);
            return;
        }
        for (u32 i = 0; i < n; i++) {
            SwapBankRegion(refs + i * sizeof(AnyRef), base, depth + 1);
        }
        break;
    }
    case detail::Util::DATATYPE_T3: { // IndexTable
        detail::BankFile::IndexTable* t = reinterpret_cast<detail::BankFile::IndexTable*>(p);
        Swap16(&t->reserved);
        const int n = t->max >= t->min ? t->max - t->min + 1 : 0;
        if (!In(t->ref, n * sizeof(AnyRef))) {
            Bad("index table", p);
            return;
        }
        for (int i = 0; i < n; i++) {
            SwapBankRegion(t->ref + i * sizeof(AnyRef), base, depth + 1);
        }
        break;
    }
    default:
        break; // DATATYPE_NONE / INVALID: nothing behind it
    }
}

// RWSD: wave sound references are relative to the data block's wsdCount
// field; wave infos are at offsets relative to the wave block
// (WsdFileReader).
void Converter::SwapWsd(u8* f) {
    detail::WsdFile::Header* h = reinterpret_cast<detail::WsdFile::Header*>(f);
    Swap32n(&h->dataBlockOffset, 4);
    if (h->fileHeader.version < NW4R_VERSION(1, 1)) {
        Bad("RWSD v1.0", f);
        return;
    }

    u8* data = At(f, h->dataBlockOffset);
    if (SwapBlockHeader(data)) {
        u8* base = data + offsetof(detail::WsdFile::DataBlock, wsdCount);
        u32 n = Table(base, sizeof(AnyRef));
        for (u32 i = 0; i < n; i++) {
            u8* w = Ref(At(base, 4 + i * sizeof(AnyRef)), base, sizeof(detail::WsdFile::Wsd));
            if (w == NULL || !First(w)) {
                continue;
            }
            detail::WsdFile::Wsd* wsd = reinterpret_cast<detail::WsdFile::Wsd*>(w);
            if (u8* p = Ref(&wsd->refWsdInfo, base, sizeof(detail::WsdFile::WsdInfo))) {
                if (First(p)) {
                    detail::WsdFile::WsdInfo* info = reinterpret_cast<detail::WsdFile::WsdInfo*>(p);
                    Swap32(&info->pitch);
                    // Envelope / randomizer tables: not read by snd, not followed.
                    Ref(&info->graphEnvTableRef, base, 0);
                    Ref(&info->randomizerTableRef, base, 0);
                    Swap32(&info->reserved);
                }
            }
            if (u8* t = Ref(&wsd->refTrackTable, base, 4)) {
                // Track infos are not read by snd: only the table is converted.
                u32 k = Table(t, sizeof(AnyRef));
                if (First(At(t, 4))) {
                    for (u32 j = 0; j < k; j++) {
                        Ref(At(t, 4 + j * sizeof(AnyRef)), base, 0);
                    }
                }
            }
            if (u8* t = Ref(&wsd->refNoteTable, base, 4)) {
                u32 k = Table(t, sizeof(AnyRef));
                if (!First(At(t, 4))) {
                    continue;
                }
                for (u32 j = 0; j < k; j++) {
                    u8* p = Ref(At(t, 4 + j * sizeof(AnyRef)), base, sizeof(detail::WsdFile::NoteInfo));
                    if (p == NULL || !First(p)) {
                        continue;
                    }
                    detail::WsdFile::NoteInfo* note = reinterpret_cast<detail::WsdFile::NoteInfo*>(p);
                    Swap32(&note->waveIndex);
                    Swap16(&note->hold);
                    Swap16(&note->PADDING_0xA);
                    Swap32(&note->pitch);
                    Ref(&note->lfoTableRef, base, 0);
                    Ref(&note->graphEnvTablevRef, base, 0);
                    Ref(&note->randomizerTableRef, base, 0);
                    Swap32(&note->reserved);
                }
            }
        }
    }

    u8* wave = At(f, h->waveBlockOffset);
    if (h->waveBlockOffset != 0 && SwapBlockHeader(wave)) {
        u8* table = wave + offsetof(detail::WsdFile::WaveBlock, waveCount);
        u32 n = Table(table, 4);
        for (u32 i = 0; i < n; i++) {
            u8* ofs = At(table, 4 + i * 4);
            Swap32(ofs);
            u32 o;
            std::memcpy(&o, ofs, 4);
            u8* w = At(wave, o);
            if (!In(w, sizeof(detail::WaveFile::WaveInfo))) {
                Bad("wave info", w);
                continue;
            }
            SwapWaveInfo(w);
        }
    }
}

// WaveInfo (WaveFileReader::ReadWaveParam): channel info offsets and the
// ADPCM info offsets are relative to the WaveInfo. The samples it points at
// (dataOffset, relative to the wave data) stay big-endian.
void Converter::SwapWaveInfo(u8* p) {
    if (!First(p)) {
        return;
    }
    detail::WaveFile::WaveInfo* w = reinterpret_cast<detail::WaveFile::WaveInfo*>(p);
    Swap16(&w->sampleRate);
    Swap16(&w->PADDING_0x6);
    Swap32n(&w->loopStart, 5); // loopStart, loopEnd, channelInfoTableOffset, dataOffset, reserved

    mWaves++;
    switch (w->format) {
    case detail::WaveFile::FORMAT_ADPCM:
        mAdpcmWaves++;
        break;
    case detail::WaveFile::FORMAT_PCM16:
        mPcm16Waves++;
        break;
    case detail::WaveFile::FORMAT_PCM8:
        mPcm8Waves++;
        break;
    }
    mLoopWaves += w->loopFlag != 0;
    mMaxChannels = w->numChannels > mMaxChannels ? w->numChannels : mMaxChannels;
    const u32 rate = (static_cast<u32>(w->sampleRate24) << 16) + w->sampleRate;
    mMinRate = rate < mMinRate ? rate : mMinRate;
    mMaxRate = rate > mMaxRate ? rate : mMaxRate;

    u8* table = At(p, w->channelInfoTableOffset);
    if (!In(table, w->numChannels * 4)) {
        Bad("wave channel table", table);
        return;
    }
    if (!First(table)) {
        return;
    }
    Swap32n(table, w->numChannels);
    for (int c = 0; c < w->numChannels; c++) {
        u32 o;
        std::memcpy(&o, table + c * 4, 4);
        u8* ci = At(p, o);
        if (!In(ci, sizeof(detail::WaveFile::WaveChannelInfo))) {
            Bad("wave channel", ci);
            continue;
        }
        if (!First(ci)) {
            continue;
        }
        detail::WaveFile::WaveChannelInfo* info = reinterpret_cast<detail::WaveFile::WaveChannelInfo*>(ci);
        Swap32n(info, 7);
        if (info->adpcmOffset != 0) {
            u8* a = At(p, info->adpcmOffset);
            if (!In(a, sizeof(detail::AdpcmInfo))) {
                Bad("ADPCM info", a);
                continue;
            }
            if (First(a)) {
                // coef[16], gain, pred_scale, yn1, yn2, loop_pred_scale,
                // loop_yn1, loop_yn2, padding: all u16.
                Swap16n(a, sizeof(detail::AdpcmInfo) / 2);
            }
        }
    }
}

} // namespace

namespace port {

void SwapSoundArchiveToNative(void* file) {
    ut::BinaryFileHeader* h = static_cast<ut::BinaryFileHeader*>(file);
    if (h == NULL || h->byteOrder != 0xFFFE) {
        return; // already native (or not a big-endian file)
    }
    if (!HasMagic(file, "RSAR")) {
        port_log("snd: %.4s is not a sound archive; not converted\n", static_cast<const char*>(file));
        return;
    }
    // Header fields first: the converter needs the size.
    Swap32(&h->signature);
    Swap16(&h->byteOrder);
    Swap16(&h->version);
    Swap32(&h->fileSize);
    Swap16(&h->headerSize);
    Swap16(&h->dataBlocks);

    Converter c(static_cast<u8*>(file), h->fileSize);
    c.SwapArchive();
    port_log("snd: brsar %lu bytes -> native: %d sounds (%d seq, %d wave, %d strm); files: %d RSEQ, %d RBNK "
             "(%d instruments), %d RWSD, %d other; %d waves (%d ADPCM, %d PCM16, %d PCM8, %d looped, "
             "up to %d channels, %lu-%lu Hz)%s\n",
             static_cast<unsigned long>(h->fileSize), c.mSounds, c.mSeqSounds, c.mWaveSounds, c.mStrmSounds,
             c.mSeqFiles, c.mBankFiles, c.mInsts, c.mWsdFiles, c.mOtherFiles, c.mWaves, c.mAdpcmWaves,
             c.mPcm16Waves, c.mPcm8Waves, c.mLoopWaves, c.mMaxChannels,
             static_cast<unsigned long>(c.mWaves ? c.mMinRate : 0), static_cast<unsigned long>(c.mMaxRate),
             c.mOk ? "" : " (with errors)");
}

} // namespace port
} // namespace snd
} // namespace nw4r
