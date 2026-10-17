/* The live view's server (step 11e): HTTP/1.0 on 127.0.0.1 only, one request at a time. The
   serve loop in gammacmd.c polls it between batches of ticks: a request that is waiting is read,
   answered and its connection closed. Only the request line is parsed: the method, the path and
   the query after '?'. The world is never written through it: the view only looks. */
#ifndef ALPHA_GAMMASERVE_H
#define ALPHA_GAMMASERVE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    intptr_t listener;  /* the listening socket (a SOCKET on Windows, a descriptor on Linux) */
    intptr_t client;    /* the request's connection while it is being answered, else -1 */
} GammaServer;

typedef struct {
    char method[16];
    char path[512];   /* up to the '?' */
    char query[512];  /* after it, or "" */
} GammaRequest;

/* Listens on 127.0.0.1:port; 0 with the reason in err if it cannot. */
int  serve_start(GammaServer *s, unsigned port, char *err, size_t len);
/* The next request waiting, without blocking: 1 and *r filled, the connection kept open for
   serve_reply; 0 if none is waiting. */
int  serve_poll(GammaServer *s, GammaRequest *r);
/* Answers the request with status 200 or 404 and the body, and closes the connection. */
void serve_reply(GammaServer *s, int status, const char *type, const void *body, size_t len);
/* Answers with lib/gamma.html, next to the current folder or the program, read from disk now;
   404 if it is missing, saying where it looked. */
void serve_page(GammaServer *s, const char *argv0);
/* The value of `name` in the query (a=1&b=2): 1 and *v if it is there and a number. */
int  serve_query_num(const GammaRequest *r, const char *name, long long *v);
void serve_stop(GammaServer *s);

#endif
