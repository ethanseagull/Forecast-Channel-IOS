// iOS-only helpers (port/ios/*.m).
#ifndef PORT_IOS_H
#define PORT_IOS_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Asks the user for the folder holding the channel's files (system folder
// picker) and copies them into the app's Documents. True if all six arrived.
bool port_ios_get_channel_files(void);

#ifdef __cplusplus
}
#endif
#endif
