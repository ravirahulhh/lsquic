/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * speed_client.c -- Speed test client: sends random data to server
 *                   and reports transfer statistics.
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <event2/event.h>

#include "lsquic.h"
#include "test_common.h"
#include "prog.h"

#include "../src/liblsquic/lsquic_logger.h"

/* Default: send 1 GB */
static uint64_t g_bytes_to_send = 1ULL * 1024 * 1024 * 1024;

/* Random data buffer */
#define SEND_BUF_SIZE (64 * 1024)  /* 64KB */
static char g_send_buf[SEND_BUF_SIZE];

struct lsquic_conn_ctx;

struct client_ctx {
    struct lsquic_conn_ctx  *conn_h;
    struct prog             *prog;
};

struct lsquic_conn_ctx {
    lsquic_conn_t       *conn;
    struct client_ctx   *client_ctx;
};

static lsquic_conn_ctx_t *
client_on_new_conn (void *stream_if_ctx, lsquic_conn_t *conn)
{
    struct client_ctx *client_ctx = stream_if_ctx;
    lsquic_conn_ctx_t *conn_h = calloc(1, sizeof(*conn_h));
    conn_h->conn = conn;
    conn_h->client_ctx = client_ctx;
    client_ctx->conn_h = conn_h;
    lsquic_conn_make_stream(conn);
    LSQ_NOTICE("New connection established");
    return conn_h;
}

static void
client_on_conn_closed (lsquic_conn_t *conn)
{
    lsquic_conn_ctx_t *conn_h = lsquic_conn_get_ctx(conn);
    LSQ_NOTICE("Connection closed");
    prog_stop(conn_h->client_ctx->prog);
    lsquic_conn_set_ctx(conn, NULL);
    free(conn_h);
}

struct lsquic_stream_ctx {
    lsquic_stream_t     *stream;
    struct client_ctx   *client_ctx;
    uint64_t             bytes_to_send;
    uint64_t             bytes_sent;
    struct timeval       start_time;
    struct timeval       end_time;
    char                 result_buf[512];
    size_t               result_off;
    int                  sending_done;
};

/* Reader functions for lsquic_stream_writef */
static size_t
random_reader_size (void *ctx)
{
    struct lsquic_stream_ctx *st_h = ctx;
    uint64_t remaining = st_h->bytes_to_send - st_h->bytes_sent;
    if (remaining > SEND_BUF_SIZE)
        return SEND_BUF_SIZE;
    return (size_t) remaining;
}

static size_t
random_reader_read (void *ctx, void *buf, size_t count)
{
    struct lsquic_stream_ctx *st_h = ctx;
    uint64_t remaining = st_h->bytes_to_send - st_h->bytes_sent;
    
    if (count > remaining)
        count = (size_t) remaining;
    if (count > SEND_BUF_SIZE)
        count = SEND_BUF_SIZE;
    
    memcpy(buf, g_send_buf, count);
    st_h->bytes_sent += count;
    
    return count;
}

static lsquic_stream_ctx_t *
client_on_new_stream (void *stream_if_ctx, lsquic_stream_t *stream)
{
    struct client_ctx *client_ctx = stream_if_ctx;
    
    if (!stream)
    {
        LSQ_NOTICE("Could not create stream");
        lsquic_conn_close(client_ctx->conn_h->conn);
        return NULL;
    }
    
    lsquic_stream_ctx_t *st_h = calloc(1, sizeof(*st_h));
    st_h->stream = stream;
    st_h->client_ctx = client_ctx;
    st_h->bytes_to_send = g_bytes_to_send;
    st_h->bytes_sent = 0;
    st_h->sending_done = 0;
    gettimeofday(&st_h->start_time, NULL);
    
    double mb = st_h->bytes_to_send / (1024.0 * 1024.0);
    LSQ_NOTICE("Starting speed test: sending %.2f MB", mb);
    
    lsquic_stream_wantwrite(stream, 1);
    return st_h;
}

static void
client_on_write (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    struct lsquic_reader reader = {
        .lsqr_read = random_reader_read,
        .lsqr_size = random_reader_size,
        .lsqr_ctx  = st_h,
    };
    
    ssize_t nw = lsquic_stream_writef(stream, &reader);
    if (nw < 0)
    {
        LSQ_ERROR("Write error: %s", strerror(errno));
        lsquic_stream_close(stream);
        return;
    }
    
    /* Progress report every 100MB */
    static uint64_t last_report = 0;
    if (st_h->bytes_sent - last_report >= 100 * 1024 * 1024)
    {
        double mb = st_h->bytes_sent / (1024.0 * 1024.0);
        double total_mb = st_h->bytes_to_send / (1024.0 * 1024.0);
        LSQ_NOTICE("Progress: %.0f / %.0f MB (%.1f%%)", 
                   mb, total_mb, (mb / total_mb) * 100.0);
        last_report = st_h->bytes_sent;
    }
    
    if (st_h->bytes_sent >= st_h->bytes_to_send)
    {
        gettimeofday(&st_h->end_time, NULL);
        
        double elapsed = (st_h->end_time.tv_sec - st_h->start_time.tv_sec) +
                        (st_h->end_time.tv_usec - st_h->start_time.tv_usec) / 1000000.0;
        double mbps = (st_h->bytes_sent * 8.0) / (elapsed * 1000000.0);
        double mb = st_h->bytes_sent / (1024.0 * 1024.0);
        
        LSQ_NOTICE("CLIENT: Sent %.2f MB in %.3f seconds = %.2f Mbps",
                   mb, elapsed, mbps);
        
        st_h->sending_done = 1;
        lsquic_stream_wantwrite(stream, 0);
        lsquic_stream_shutdown(stream, 1);  /* Done writing */
        lsquic_stream_wantread(stream, 1);  /* Wait for server response */
    }
}

static void
client_on_read (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    char buf[256];
    ssize_t nr;
    
    nr = lsquic_stream_read(stream, buf, sizeof(buf) - 1);
    if (nr > 0)
    {
        buf[nr] = '\0';
        /* Accumulate result */
        if (st_h->result_off + nr < sizeof(st_h->result_buf))
        {
            memcpy(st_h->result_buf + st_h->result_off, buf, nr);
            st_h->result_off += nr;
            st_h->result_buf[st_h->result_off] = '\0';
        }
    }
    else if (nr == 0)
    {
        /* EOF - server finished */
        LSQ_NOTICE("SERVER: %s", st_h->result_buf);
        lsquic_stream_shutdown(stream, 0);
        lsquic_conn_close(st_h->client_ctx->conn_h->conn);
    }
    else
    {
        LSQ_WARN("Read error: %s", strerror(errno));
        lsquic_stream_close(stream);
    }
}

static void
client_on_close (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    LSQ_NOTICE("Stream closed, total sent: %"PRIu64" bytes", st_h->bytes_sent);
    free(st_h);
}

const struct lsquic_stream_if client_speed_stream_if = {
    .on_new_conn            = client_on_new_conn,
    .on_conn_closed         = client_on_conn_closed,
    .on_new_stream          = client_on_new_stream,
    .on_read                = client_on_read,
    .on_write               = client_on_write,
    .on_close               = client_on_close,
};

static void
init_random_buffer (void)
{
    /* Initialize with pseudo-random data */
    srand((unsigned int) time(NULL));
    for (size_t i = 0; i < SEND_BUF_SIZE; i++)
        g_send_buf[i] = (char) rand();
}

static void
usage (const char *prog)
{
    const char *const slash = strrchr(prog, '/');
    if (slash)
        prog = slash + 1;
    printf(
"Usage: %s [opts]\n"
"\n"
"Speed test client - sends random data to server and reports statistics.\n"
"\n"
"Options:\n"
"   -b BYTES    Number of bytes to send (default: 1GB)\n"
"               Supports suffixes: K, M, G (e.g., -b 500M, -b 2G)\n"
            , prog);
}

static uint64_t
parse_size (const char *str)
{
    char *end;
    uint64_t val = strtoull(str, &end, 10);
    
    if (*end == 'K' || *end == 'k')
        val *= 1024;
    else if (*end == 'M' || *end == 'm')
        val *= 1024 * 1024;
    else if (*end == 'G' || *end == 'g')
        val *= 1024ULL * 1024 * 1024;
    
    return val;
}

int
main (int argc, char **argv)
{
    int opt, s;
    struct sport_head sports;
    struct prog prog;
    struct client_ctx client_ctx;

    memset(&client_ctx, 0, sizeof(client_ctx));
    client_ctx.prog = &prog;

    TAILQ_INIT(&sports);
    prog_init(&prog, 0, &sports, &client_speed_stream_if, &client_ctx);
    prog.prog_api.ea_alpn = "speed";

    while (-1 != (opt = getopt(argc, argv, PROG_OPTS "hb:")))
    {
        switch (opt) {
        case 'b':
            g_bytes_to_send = parse_size(optarg);
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

    init_random_buffer();
    
    double mb = g_bytes_to_send / (1024.0 * 1024.0);
    LSQ_NOTICE("Will send %.2f MB of random data", mb);

    if (0 != prog_prep(&prog))
    {
        LSQ_ERROR("could not prep");
        exit(EXIT_FAILURE);
    }

    if (0 != prog_connect(&prog, NULL, 0))
    {
        LSQ_ERROR("could not connect");
        exit(EXIT_FAILURE);
    }

    LSQ_DEBUG("Speed test client entering event loop");

    s = prog_run(&prog);
    prog_cleanup(&prog);

    exit(0 == s ? EXIT_SUCCESS : EXIT_FAILURE);
}
