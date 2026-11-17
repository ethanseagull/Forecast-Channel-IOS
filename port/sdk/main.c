// Host entry point: SDL setup, the data directory, then the channel's own
// main loop (src/main.cpp, channel_main under FORECAST_PORT).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdlib.h>

#include <port_log.h>
#include <port_mem.h>
#include <port_import.h>
#include <port_paths.h>
#include <port_platform.h>

void channel_main(void);

static char sDataDir[1024];

const char* port_data_dir(void) { return sDataDir; }

void port_paths_init(void) {
    const char* env = SDL_getenv("FORECAST_DATA");
    if (env != NULL && env[0] != 0) {
        size_t n = SDL_strlcpy(sDataDir, env, sizeof(sDataDir) - 1);
        if (n > 0 && sDataDir[n - 1] != '/' && sDataDir[n - 1] != '\\') {
            sDataDir[n] = '/';
            sDataDir[n + 1] = 0;
        }
    } else {
#ifdef SDL_PLATFORM_IOS
        // The app's Documents folder: the user copies the channel's files
        // there with the Files app (UIFileSharingEnabled).
        const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
        SDL_snprintf(sDataDir, sizeof(sDataDir), "%s", docs != NULL ? docs : SDL_GetPrefPath("ForecastOS", "ForecastOS"));
#else
        SDL_snprintf(sDataDir, sizeof(sDataDir), "%sdata/", SDL_GetBasePath());
#endif
    }
    SDL_CreateDirectory(sDataDir);
}

// Called by every generated stub (port/sdk/autostubs.c). Logs each unported
// SDK function the first time it is hit; the stub then returns 0.
#define PORT_STUB_SEEN_MAX 1024
static const char* sSeen[PORT_STUB_SEEN_MAX];
static int sSeenCount;

void port_stub_hit(const char* name) {
    for (int i = 0; i < sSeenCount; i++) {
        if (sSeen[i] == name) {
            return;
        }
    }
    if (sSeenCount < PORT_STUB_SEEN_MAX) {
        sSeen[sSeenCount++] = name;
    }
    port_log("stub: %s\n", name);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    port_mark_game_thread();
    port_paths_init();
    port_log_open();
    port_log("ForecastOS: Forecast Channel host port (%s, %d-bit)\n", SDL_GetPlatform(), (int)sizeof(void*) * 8);
    port_log("data: %s\n", port_data_dir());
    port_import_contents();
    port_load_dol_data();
    if (!port_platform_init()) {
        return 1;
    }
    channel_main();
    port_platform_quit();
    return 0;
}
