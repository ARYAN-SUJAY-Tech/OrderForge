/*
 * Spike 1: a C FastCGI responder reached through Apache (mod_proxy_fcgi).
 *
 * The process stays alive and serves many requests on one listening socket, which is
 * why of-api will not pay a fork per request. Two threads share the socket and each
 * calls FCGX_Accept_r, the same pattern the pre-forked of-api workers will use.
 *
 *   usage: fcgi_hello [bind=127.0.0.1:9000]
 *   then:  curl http://localhost/api/hello   (with conf/apache-spike.conf enabled)
 */
#include "common/log.h"

#include <fcgiapp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#define THREADS 2

static int listen_fd;
static atomic_long served;

static void *serve(void *arg)
{
    long id = (long)arg;
    FCGX_Request req;
    if (FCGX_InitRequest(&req, listen_fd, 0) != 0)
        return NULL;

    while (FCGX_Accept_r(&req) >= 0) {
        const char *method = FCGX_GetParam("REQUEST_METHOD", req.envp);
        const char *uri = FCGX_GetParam("REQUEST_URI", req.envp);
        long n = atomic_fetch_add(&served, 1) + 1;

        FCGX_FPrintF(req.out,
                     "Content-Type: application/json\r\n\r\n"
                     "{\"hello\":\"orderforge\",\"pid\":%d,\"thread\":%ld,"
                     "\"request\":%ld}\n",
                     (int)getpid(), id, n);
        LOG_INFO("request", "%s %s served by thread %ld", method ? method : "?",
                 uri ? uri : "?", id);
        FCGX_Finish_r(&req);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *bind = argc > 1 ? argv[1] : "127.0.0.1:9000";
    of_log_open(NULL, "fcgi-hello", OF_LOG_INFO);

    if (FCGX_Init() != 0) {
        LOG_ERROR("init", "FCGX_Init failed");
        return 1;
    }
    listen_fd = FCGX_OpenSocket(bind, 128);
    if (listen_fd < 0) {
        LOG_ERROR("listen", "cannot listen on %s", bind);
        return 1;
    }
    LOG_INFO("listen", "FastCGI listening on %s with %d threads", bind, THREADS);

    pthread_t t[THREADS];
    for (long i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, serve, (void *)i);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    return 0;
}
