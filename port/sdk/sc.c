// SC (system configuration): the Wii settings the channel reads, answered
// from the host's locale where possible.
#include <SDL3/SDL.h>

#include <revolution/SC.h>

#include <port_log.h>

static BOOL sInit;
static u8 sLanguage = SC_LANG_EN;
static u32 sAddressID = 0xFFFFFFFF;

void SCInit(void) {
    int count = 0;
    if (sInit) {
        return;
    }
    sInit = TRUE;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    if (locales != NULL && count > 0 && locales[0]->language != NULL) {
        const char* l = locales[0]->language;
        if (SDL_strcmp(l, "ja") == 0) {
            sLanguage = SC_LANG_JP;
        } else if (SDL_strcmp(l, "fr") == 0) {
            sLanguage = SC_LANG_FR;
        } else if (SDL_strcmp(l, "de") == 0) {
            sLanguage = SC_LANG_DE;
        } else if (SDL_strcmp(l, "it") == 0) {
            sLanguage = SC_LANG_IT;
        } else if (SDL_strcmp(l, "es") == 0) {
            sLanguage = SC_LANG_SP;
        } else if (SDL_strcmp(l, "nl") == 0) {
            sLanguage = SC_LANG_NL;
        }
    }
    SDL_free(locales);
    // The Wii's simple address ID is country:province in the top two bytes.
    // Until the app has a location setting: USA (49), no province.
    sAddressID = 49u << 24;
    port_log("sc: language %u, address %08x\n", sLanguage, sAddressID);
}

u32 SCCheckStatus(void) { return SC_STATUS_OK; }

u8 SCGetLanguage(void) {
    SCInit();
    return sLanguage;
}

// The channel is the USA release.
s8 SCGetProductArea(void) { return SC_AREA_USA; }

u32 SCGetSimpleAddressID(void) {
    SCInit();
    return sAddressID;
}

u8 SCGetAspectRatio(void) { return SC_ASPECT_WIDE; }

u8 SCGetProgressiveMode(void) { return 1; }

u8 SCGetEuRgb60Mode(void) { return 0; }
