/* The live view's server (step 11e), see gammaserve.h. Winsock2 on Windows, BSD sockets on
   Linux; the listener is bound to 127.0.0.1 and never to a LAN address. Every socket is
   non-blocking and waited for with select(), so a browser that opens a connection and sends
   nothing (they do, ahead of time) cannot stop the ticks: it is dropped after half a second. */
#include "gammaserve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
typedef SOCKET sock_t;
#define BAD_SOCKET INVALID_SOCKET
#define SHUT_WR SD_SEND
#define MSG_NOSIGNAL 0
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCKET (-1)
#endif

enum { REQUEST_MAX = 8192 };  /* a request longer than this is cut: the request line comes first */

static void close_socket(sock_t s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static void set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
}

/* the last socket call failed only because it would have had to wait */
static int would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/* 1 if s is readable (writable: 1 if it can take data) within ms milliseconds */
static int ready(sock_t s, int writable, int ms)
{
    fd_set set;
    struct timeval tv;
    FD_ZERO(&set);
    FD_SET(s, &set);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = ms % 1000 * 1000;
    return select((int)s + 1, writable ? NULL : &set, writable ? &set : NULL, NULL, &tv) == 1;
}

int serve_start(GammaServer *sv, unsigned port, char *err, size_t len)
{
    sv->listener = sv->client = -1;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        snprintf(err, len, "Winsock did not start");
        return 0;
    }
#endif
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == BAD_SOCKET) {
        snprintf(err, len, "cannot make a socket");
        return 0;
    }
    int one = 1;  /* on Windows SO_REUSEADDR would let a second server take the same port */
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s, 16) != 0) {
        snprintf(err, len, "cannot listen on 127.0.0.1:%u (is the port in use?)", port);
        close_socket(s);
        return 0;
    }
    set_nonblocking(s);
    sv->listener = (intptr_t)s;
    return 1;
}

/* Reads one request from c into buf: up to the blank line after the headers, REQUEST_MAX bytes
   or half a second, whichever comes first. Returns the length. */
static size_t read_request(sock_t c, char *buf)
{
    size_t len = 0;
    double deadline = now_seconds() + 0.5;
    while (len < REQUEST_MAX) {
        if (!ready(c, 0, 50)) {
            if (now_seconds() > deadline)
                break;
            continue;
        }
        int n = (int)recv(c, buf + len, (int)(REQUEST_MAX - len), 0);
        if (n == 0 || (n < 0 && !would_block()))
            break;
        if (n < 0)
            continue;
        len += (size_t)n;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n"))
            break;
    }
    buf[len] = '\0';
    return len;
}

/* the request line "GET /path?query HTTP/1.1" into r (the widths are the fields' sizes less
   one); 0 if there is none */
static int parse_request(const char *buf, GammaRequest *r)
{
    if (sscanf(buf, "%15s %511s", r->method, r->path) != 2)
        return 0;
    char *q = strchr(r->path, '?');
    if (q)
        *q++ = '\0';
    snprintf(r->query, sizeof r->query, "%s", q ? q : "");
    return 1;
}

int serve_poll(GammaServer *sv, GammaRequest *r)
{
    static char buf[REQUEST_MAX + 1];
    sock_t ls = (sock_t)sv->listener;
    for (;;) {
        if (!ready(ls, 0, 0))
            return 0;
        sock_t c = accept(ls, NULL, NULL);
        if (c == BAD_SOCKET)
            return 0;
        set_nonblocking(c);
        read_request(c, buf);
        if (parse_request(buf, r)) {
            sv->client = (intptr_t)c;
            return 1;
        }
        close_socket(c);  /* nothing came: a connection opened ahead of time, or noise */
    }
}

/* sends all of p, waiting for the socket as needed, for at most two seconds in all */
static int send_all(sock_t c, const char *p, size_t len)
{
    double deadline = now_seconds() + 2;
    while (len) {
        if (!ready(c, 1, 100)) {
            if (now_seconds() > deadline)
                return 0;
            continue;
        }
        int n = (int)send(c, p, (int)(len < 1 << 20 ? len : 1 << 20), MSG_NOSIGNAL);
        if (n < 0 && !would_block())
            return 0;
        if (n > 0) {
            p += n;
            len -= (size_t)n;
        }
    }
    return 1;
}

void serve_reply(GammaServer *sv, int status, const char *type, const void *body, size_t len)
{
    sock_t c = (sock_t)sv->client;
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n",
                     status, status == 200 ? "OK" : "Not Found", type, len);
    if (send_all(c, head, (size_t)n))
        send_all(c, body, len);
    shutdown(c, SHUT_WR);
    close_socket(c);
    sv->client = -1;
}

/* lib/ next to the current folder, or next to the program's own folder (as alpha cube finds it) */
static void lib_dir(const char *argv0, char *buf, size_t len)
{
    FILE *f = fopen("lib/gamma.html", "r");
    if (f) {
        fclose(f);
        snprintf(buf, len, "lib");
        return;
    }
    const char *slash = NULL;
    for (const char *c = argv0; *c; c++)
        if (*c == '/' || *c == '\\')
            slash = c;
    if (slash)
        snprintf(buf, len, "%.*s/../lib", (int)(slash - argv0), argv0);
    else
        snprintf(buf, len, "../lib");
}

void serve_page(GammaServer *sv, const char *argv0)
{
    char lib[512], path[600], msg[700];
    lib_dir(argv0, lib, sizeof lib);
    snprintf(path, sizeof path, "%s/gamma.html", lib);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(msg, sizeof msg, "the page is missing: looked for %s\n", path);
        serve_reply(sv, 404, "text/plain; charset=utf-8", msg, strlen(msg));
        return;
    }
    size_t cap = 1 << 16, len = 0, n;
    char *page = malloc(cap);
    while (page && (n = fread(page + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            char *more = realloc(page, cap *= 2);
            if (!more)
                free(page);
            page = more;
        }
    }
    fclose(f);
    if (page)
        serve_reply(sv, 200, "text/html; charset=utf-8", page, len);
    else
        serve_reply(sv, 404, "text/plain; charset=utf-8", "out of memory\n", 14);
    free(page);
}

int serve_query_num(const GammaRequest *r, const char *name, long long *v)
{
    size_t n = strlen(name);
    for (const char *q = r->query; *q; q++) {
        if ((q == r->query || q[-1] == '&') && !strncmp(q, name, n) && q[n] == '=') {
            char *end;
            *v = strtoll(q + n + 1, &end, 10);
            return end > q + n + 1 && (*end == '\0' || *end == '&');
        }
    }
    return 0;
}

void serve_stop(GammaServer *sv)
{
    if (sv->client >= 0)
        close_socket((sock_t)sv->client);
    if (sv->listener >= 0)
        close_socket((sock_t)sv->listener);
    sv->listener = sv->client = -1;
#ifdef _WIN32
    WSACleanup();
#endif
}
