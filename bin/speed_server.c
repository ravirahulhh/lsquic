/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * speed_server.c -- Speed test server: receives data from client and
 *                   reports statistics back.
 */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>

#include <event2/event.h>

#include "lsquic.h"
#include "test_common.h"
#include "../src/liblsquic/lsquic_hash.h"
#include "test_cert.h"
#include "prog.h"

#include "../src/liblsquic/lsquic_logger.h"

struct lsquic_conn_ctx;

struct server_ctx {
    TAILQ_HEAD(, lsquic_conn_ctx)   conn_ctxs;
    struct sport_head sports;
    struct prog *prog;
    int n_conn;
};

struct lsquic_conn_ctx {
    TAILQ_ENTRY(lsquic_conn_ctx)    next_connh;
    lsquic_conn_t       *conn;
    struct server_ctx   *server_ctx;
};

static lsquic_conn_ctx_t *
server_on_new_conn (void *stream_if_ctx, lsquic_conn_t *conn)
{
    struct server_ctx *server_ctx = stream_if_ctx;
    lsquic_conn_ctx_t *conn_h = calloc(1, sizeof(*conn_h));
    conn_h->conn = conn;
    conn_h->server_ctx = server_ctx;
    TAILQ_INSERT_TAIL(&server_ctx->conn_ctxs, conn_h, next_connh);
    LSQ_NOTICE("New connection!");
    return conn_h;
}

static void
server_on_conn_closed (lsquic_conn_t *conn)
{
    lsquic_conn_ctx_t *conn_h = lsquic_conn_get_ctx(conn);
    
    if (conn_h->server_ctx->n_conn)
    {
        --conn_h->server_ctx->n_conn;
        LSQ_NOTICE("Connection closed, remaining: %d", conn_h->server_ctx->n_conn);
        if (0 == conn_h->server_ctx->n_conn)
            prog_stop(conn_h->server_ctx->prog);
    }
    else
        LSQ_NOTICE("Connection closed");
    
    TAILQ_REMOVE(&conn_h->server_ctx->conn_ctxs, conn_h, next_connh);
    lsquic_conn_set_ctx(conn, NULL);
    free(conn_h);
}

struct lsquic_stream_ctx {
    lsquic_stream_t     *stream;
    struct server_ctx   *server_ctx;
    uint64_t             bytes_received;
    uint64_t             last_report_bytes;      /* For progress reporting */
    struct timeval       start_time;
    struct timeval       last_report_time;       /* For real-time speed */
    struct timeval       end_time;
    char                 result_buf[256];
    size_t               result_len;
    size_t               result_off;
};

static struct lsquic_conn_ctx *
find_conn_h (const struct server_ctx *server_ctx, lsquic_stream_t *stream)
{
    struct lsquic_conn_ctx *conn_h;
    lsquic_conn_t *conn;

    conn = lsquic_stream_conn(stream);
    TAILQ_FOREACH(conn_h, &server_ctx->conn_ctxs, next_connh)
        if (conn_h->conn == conn)
            return conn_h;
    return NULL;
}

static lsquic_stream_ctx_t *
server_on_new_stream (void *stream_if_ctx, lsquic_stream_t *stream)
{
    lsquic_stream_ctx_t *st_h = calloc(1, sizeof(*st_h));
    st_h->stream = stream;
    st_h->server_ctx = stream_if_ctx;
    st_h->bytes_received = 0;
    st_h->last_report_bytes = 0;
    gettimeofday(&st_h->start_time, NULL);
    st_h->last_report_time = st_h->start_time;
    lsquic_stream_wantread(stream, 1);
    LSQ_NOTICE("New stream, starting speed test receive");
    return st_h;
}

/* Report interval: every 100MB or 1 second */
#define REPORT_INTERVAL_BYTES (100 * 1024 * 1024)
#define REPORT_INTERVAL_USEC  (1000000)

static void
server_on_read (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    char buf[0x4000];  /* 16KB buffer */
    ssize_t nr;
    struct timeval now;

    nr = lsquic_stream_read(stream, buf, sizeof(buf));
    if (nr > 0)
    {
        st_h->bytes_received += nr;
        
        /* Check if we should print progress */
        gettimeofday(&now, NULL);
        uint64_t bytes_since_report = st_h->bytes_received - st_h->last_report_bytes;
        long usec_since_report = (now.tv_sec - st_h->last_report_time.tv_sec) * 1000000 +
                                 (now.tv_usec - st_h->last_report_time.tv_usec);
        
        if (bytes_since_report >= REPORT_INTERVAL_BYTES || usec_since_report >= REPORT_INTERVAL_USEC)
        {
            /* Calculate real-time speed (since last report) */
            double interval_sec = usec_since_report / 1000000.0;
            double realtime_mbps = (bytes_since_report * 8.0) / (interval_sec * 1000000.0);
            
            /* Calculate average speed (since start) */
            double total_elapsed = (now.tv_sec - st_h->start_time.tv_sec) +
                                  (now.tv_usec - st_h->start_time.tv_usec) / 1000000.0;
            double avg_mbps = (st_h->bytes_received * 8.0) / (total_elapsed * 1000000.0);
            
            double mb_received = st_h->bytes_received / (1024.0 * 1024.0);
            
            LSQ_NOTICE("Progress: %.1f MB | Real-time: %.2f Mbps | Avg: %.2f Mbps",
                       mb_received, realtime_mbps, avg_mbps);
            
            st_h->last_report_bytes = st_h->bytes_received;
            st_h->last_report_time = now;
        }
    }
    else if (nr == 0)
    {
        /* EOF - client finished sending */
        gettimeofday(&st_h->end_time, NULL);
        
        double elapsed = (st_h->end_time.tv_sec - st_h->start_time.tv_sec) +
                        (st_h->end_time.tv_usec - st_h->start_time.tv_usec) / 1000000.0;
        double mbps = (st_h->bytes_received * 8.0) / (elapsed * 1000000.0);
        double mb = st_h->bytes_received / (1024.0 * 1024.0);
        
        LSQ_NOTICE("=== Transfer Complete ===");
        LSQ_NOTICE("Received %.2f MB in %.3f seconds", mb, elapsed);
        LSQ_NOTICE("Average speed: %.2f Mbps (%.2f MB/s)", mbps, mbps / 8.0);
        
        /* Prepare result to send back to client */
        st_h->result_len = snprintf(st_h->result_buf, sizeof(st_h->result_buf),
            "RESULT: bytes=%"PRIu64" time=%.3fs speed=%.2fMbps\n",
            st_h->bytes_received, elapsed, mbps);
        st_h->result_off = 0;
        
        lsquic_stream_wantread(stream, 0);
        lsquic_stream_wantwrite(stream, 1);
    }
    else
    {
        LSQ_WARN("Error reading from stream: %s", strerror(errno));
        lsquic_stream_close(stream);
    }
}

static void
server_on_write (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    ssize_t nw;
    size_t towrite = st_h->result_len - st_h->result_off;
    
    nw = lsquic_stream_write(stream, st_h->result_buf + st_h->result_off, towrite);
    if (nw > 0)
    {
        st_h->result_off += nw;
        if (st_h->result_off >= st_h->result_len)
        {
            lsquic_stream_wantwrite(stream, 0);
            lsquic_stream_shutdown(stream, 1);
        }
    }
    else
    {
        LSQ_WARN("Error writing to stream: %s", strerror(errno));
        lsquic_stream_close(stream);
    }
}

static void
server_on_close (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    LSQ_NOTICE("Stream closed, total received: %"PRIu64" bytes", st_h->bytes_received);
    free(st_h);
}

const struct lsquic_stream_if server_speed_stream_if = {
    .on_new_conn            = server_on_new_conn,
    .on_conn_closed         = server_on_conn_closed,
    .on_new_stream          = server_on_new_stream,
    .on_read                = server_on_read,
    .on_write               = server_on_write,
    .on_close               = server_on_close,
};

static void
usage (const char *prog)
{
    const char *const slash = strrchr(prog, '/');
    if (slash)
        prog = slash + 1;
    printf(
"Usage: %s [opts]\n"
"\n"
"Speed test server - receives data and reports transfer statistics.\n"
"\n"
"Options:\n"
"   -n N        Exit after N connections\n"
            , prog);
}

int
main (int argc, char **argv)
{
    int opt, s;
    struct prog prog;
    struct server_ctx server_ctx;

    memset(&server_ctx, 0, sizeof(server_ctx));
    TAILQ_INIT(&server_ctx.conn_ctxs);
    server_ctx.prog = &prog;
    TAILQ_INIT(&server_ctx.sports);
    prog_init(&prog, LSENG_SERVER, &server_ctx.sports,
                                    &server_speed_stream_if, &server_ctx);

    while (-1 != (opt = getopt(argc, argv, PROG_OPTS "hn:")))
    {
        switch (opt) {
        case 'n':
            server_ctx.n_conn = atoi(optarg);
            break;
        case 'h':
            usage(argv[0]);
            prog_print_common_options(&prog, stdout);
            exit(0);
        default:
            if (0 != prog_set_opt(&prog, opt, optarg))
                exit(1);
        }
    }

    add_alpn("speed");
    if (0 != prog_prep(&prog))
    {
        LSQ_ERROR("could not prep");
        exit(EXIT_FAILURE);
    }

    LSQ_DEBUG("Speed test server entering event loop");

    s = prog_run(&prog);
    prog_cleanup(&prog);

    exit(0 == s ? EXIT_SUCCESS : EXIT_FAILURE);
}
