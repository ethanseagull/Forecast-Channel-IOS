// Plain HTTP GET (port/sdk/http.c).
#ifndef PORT_HTTP_H
#define PORT_HTTP_H
#ifdef __cplusplus
extern "C" {
#endif

// Fetches url (http:// only). On a response, returns 1 with the body in
// *outData (malloc'd, free it) and the HTTP status in *outStatus; returns 0
// if no response arrived (no network, DNS, timeout).
int port_http_get(const char* url, unsigned char** outData, unsigned* outSize, int* outStatus);

#ifdef __cplusplus
}
#endif
#endif
