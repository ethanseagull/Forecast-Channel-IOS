#ifndef PORT_LOG_H
#define PORT_LOG_H
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
void port_log_open(void);
void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void port_log_v(const char* fmt, va_list ap);
#ifdef __cplusplus
}
#endif
#endif
