// The Wii HOME Button menu (HBM) is not part of the port: the 3DS has its own
// HOME menu, handled by the system through aptMainLoop() (port/sdk/vi.c).
// This replaces the decomp's src/HomeButton.cpp with an always-ready, never
// open menu, so scenes that check it carry on as if the user never pressed
// HOME. The operations manual it could open (Opera) is not available.
#include <channel/HomeButton.h>

#include <cstring>

HomeButton::HomeButton(u32 manualContent, const char* manualPath, const char* manualPage,
                       MEMAllocator* allocator1, MEMAllocator* allocator2, void* workBuf) {
    (void)workBuf;
    std::memset(this, 0, sizeof(*this));
    mManualContent = manualContent;
    mManualPath = manualPath;
    mManualPage = manualPage;
    mAllocator1 = allocator1;
    mAllocator2 = allocator2;
    mResult = HOME_BUTTON_RESULT_NONE;
    mIsReady = TRUE;
}

HomeButton::~HomeButton() {}

void HomeButton::Init() {}

s32 HomeButton::Calc() {
    mResult = HOME_BUTTON_RESULT_NONE;
    mFrameCount++;
    return mResult;
}

void HomeButton::Draw() {}

BOOL HomeButton::OpenManual() {
    return FALSE;
}

void HomeButton::OnReset() {}
