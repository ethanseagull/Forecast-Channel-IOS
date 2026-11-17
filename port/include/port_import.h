// Importing the user's channel files and reading the DOL (port/sdk/import.c).
#ifndef PORT_IMPORT_H
#define PORT_IMPORT_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Unpacks the channel's archives (and copies the DOL) dropped into the data
// folder into <data>/content, where they are not already.
// Returns how many of the six files are still missing.
int port_import_contents(void);

// Copies size bytes at Wii address addr out of the DOL.
bool port_dol_read(uint32_t addr, void* dst, uint32_t size);
void port_dol_release(void);

// Fills the tables the decomp compiles in from the DOL on the Wii.
bool port_load_dol_data(void);

#ifdef __cplusplus
}
#endif
#endif
