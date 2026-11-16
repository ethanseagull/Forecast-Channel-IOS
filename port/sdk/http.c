// A minimal HTTP client: GET over a plain socket, HTTP/1.0 (so no chunked
// encoding; the server closes the connection after the body). Enough for
// WiiConnect24's plain-HTTP downloads. Raw sockets also need no iOS App
// Transport Security exception.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

#include <port_http.h>
#include <port_log.h>

static int sInit;

static int net_init(void) {
#ifdef _WIN32
    if (!sInit) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return 0;
        }
    }
#endif
    sInit = 1;
    return 1;
}

static sock_t connect_to(const char* host, const char* port) {
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        port_log("http: cannot resolve %s\n", host);
        return SOCK_BAD;
    }
    sock_t s = SOCK_BAD;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == SOCK_BAD) {
            continue;
        }
        // 15 s receive / send timeouts.
#ifdef _WIN32
        DWORD tv = 15000;
#else
        struct timeval tv = {15, 0};
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            break;
        }
        sock_close(s);
        s = SOCK_BAD;
    }
    freeaddrinfo(res);
    if (s == SOCK_BAD) {
        port_log("http: cannot connect to %s:%s\n", host, port);
    }
    return s;
}

int port_http_get(const char* url, unsigned char** outData, unsigned* outSize, int* outStatus) {
    *outData = NULL;
    *outSize = 0;
    *outStatus = 0;
    if (strncmp(url, "http://", 7) != 0) {
        port_log("http: only http:// is supported (%s)\n", url);
        return 0;
    }
    char host[256], port[8] = "80";
    const char* h = url + 7;
    const char* path = strchr(h, '/');
    size_t hostLen = path != NULL ? (size_t)(path - h) : strlen(h);
    if (hostLen == 0 || hostLen >= sizeof(host)) {
        return 0;
    }
    memcpy(host, h, hostLen);
    host[hostLen] = '\0';
    char* colon = strchr(host, ':');
    if (colon != NULL) {
        *colon = '\0';
        snprintf(port, sizeof(port), "%s", colon + 1);
    }
    if (path == NULL) {
        path = "/";
    }
    if (!net_init()) {
        return 0;
    }
    sock_t s = connect_to(host, port);
    if (s == SOCK_BAD) {
        return 0;
    }

    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ForecastOS\r\nAccept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     path, host);
    if (n <= 0 || n >= (int)sizeof(req) || send(s, req, n, 0) != n) {
        port_log("http: send failed (%s)\n", url);
        sock_close(s);
        return 0;
    }

    size_t cap = 64 * 1024, len = 0;
    unsigned char* buf = (unsigned char*)malloc(cap);
    for (;;) {
        if (len == cap) {
            if (cap >= 64u * 1024 * 1024) {
                port_log("http: response too large (%s)\n", url);
                free(buf);
                sock_close(s);
                return 0;
            }
            cap *= 2;
            buf = (unsigned char*)realloc(buf, cap);
        }
        int r = recv(s, (char*)buf + len, (int)(cap - len), 0);
        if (r < 0) {
            port_log("http: receive failed (%s)\n", url);
            free(buf);
            sock_close(s);
            return 0;
        }
        if (r == 0) {
            break;
        }
        len += (size_t)r;
    }
    sock_close(s);

    // Status line and the end of the headers.
    unsigned char* body = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            body = buf + i + 4;
            break;
        }
    }
    int status = 0;
    if (body == NULL || sscanf((const char*)buf, "HTTP/%*d.%*d %d", &status) != 1) {
        port_log("http: malformed response (%s)\n", url);
        free(buf);
        return 0;
    }
    *outStatus = status;
    size_t bodyLen = len - (size_t)(body - buf);
    memmove(buf, body, bodyLen);
    *outData = buf;
    *outSize = (unsigned)bodyLen;
    return 1;
}
