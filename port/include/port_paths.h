// Where the port keeps the user's imported channel files and its own output.
//
// Data directory (ends in a path separator):
//   Windows: $FORECAST_DATA if set, else <exe dir>/data/
//   iOS:     the app's Documents folder (set up by the iOS shell)
// Layout inside it:
//   content/0000000N/<path>   the channel's U8 contents, unpacked
//   boot.log                  the port log
#ifndef PORT_PATHS_H
#define PORT_PATHS_H
#ifdef __cplusplus
extern "C" {
#endif
void port_paths_init(void);
const char* port_data_dir(void);
#ifdef __cplusplus
}
#endif
#endif
