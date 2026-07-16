/*
 * audio_test — deterministic unit tests for the QEMU audio wire parser.
 *
 * The parser is the one protocol decoder we wrote ourselves, so it gets direct
 * table-driven coverage (run under the ASan/UBSan preset) in addition to the
 * fuzz harness.
 */
#include <stdio.h>
#include <string.h>

#include "core/qemu_audio.h"

/* A read function over a fixed memory buffer. */
typedef struct { const uint8_t *p; uint32_t remaining; } mem_reader;
static bool mem_read(void *ctx, void *dst, uint32_t n)
{
    mem_reader *m = ctx;
    if (n > m->remaining)
        return false;
    memcpy(dst, m->p, n);
    m->p += n;
    m->remaining -= n;
    return true;
}

static int g_begin, g_end;
static uint64_t g_data_bytes;
static void s_begin(void *u) { (void)u; g_begin++; }
static void s_end(void *u)   { (void)u; g_end++; }
static void s_data(void *u, const uint8_t *p, uint32_t n)
{ (void)u; (void)p; g_data_bytes += n; }

static int failures;
static void check(const char *name, int cond)
{
    printf("  %-44s %s\n", name, cond ? "ok" : "FAIL");
    if (!cond) failures++;
}

/* Build a server audio message body (submsg=1 + op + optional data). */
static uint32_t msg_begin(uint8_t *b)  { b[0]=1; b[1]=0; b[2]=QA_SERVER_BEGIN; return 3; }
static uint32_t msg_end(uint8_t *b)    { b[0]=1; b[1]=0; b[2]=QA_SERVER_END;   return 3; }
static uint32_t msg_data(uint8_t *b, uint32_t len)
{
    b[0]=1; b[1]=0; b[2]=QA_SERVER_DATA;
    b[3]=(uint8_t)(len>>24); b[4]=(uint8_t)(len>>16);
    b[5]=(uint8_t)(len>>8);  b[6]=(uint8_t)len;
    for (uint32_t i=0;i<len;i++) b[7+i]=(uint8_t)i;
    return 7+len;
}

static bool run(qa_state *st, const uint8_t *buf, uint32_t len)
{
    mem_reader m = { buf, len };
    qa_sink sink = { NULL, s_begin, s_end, s_data };
    return qemu_audio_parse_server_msg(st, mem_read, &m, &sink);
}

int main(void)
{
    uint8_t b[8192];
    qa_state st;

    printf("qemu audio parser tests:\n");

    /* Normal begin -> data -> end delivers the data. */
    memset(&st,0,sizeof st); g_begin=g_end=0; g_data_bytes=0;
    check("begin accepted",           run(&st, b, msg_begin(b)) && g_begin==1);
    check("data while playing",       run(&st, b, msg_data(b, 1000)) && g_data_bytes==1000);
    check("end accepted",             run(&st, b, msg_end(b)) && g_end==1);

    /* Data BEFORE begin is consumed for sync but NOT delivered. */
    memset(&st,0,sizeof st); g_begin=g_end=0; g_data_bytes=0;
    check("data before begin dropped", run(&st, b, msg_data(b, 500)) && g_data_bytes==0);

    /* Oversize length is rejected (fail closed). */
    memset(&st,0,sizeof st);
    b[0]=1; b[1]=0; b[2]=QA_SERVER_DATA;
    b[3]=0xFF; b[4]=0xFF; b[5]=0xFF; b[6]=0xFF; /* ~4 GiB */
    check("oversize length rejected", !run(&st, b, 7));

    /* Unknown op is rejected. */
    memset(&st,0,sizeof st);
    b[0]=1; b[1]=0; b[2]=99;
    check("unknown op rejected",      !run(&st, b, 3));

    /* Wrong submessage type rejected. */
    memset(&st,0,sizeof st);
    b[0]=7; b[1]=0; b[2]=QA_SERVER_BEGIN;
    check("bad submessage rejected",  !run(&st, b, 3));

    /* Truncated DATA payload rejected. */
    memset(&st,0,sizeof st); run(&st, b, msg_begin(b));
    { uint32_t n = msg_data(b, 100); check("truncated data rejected", !run(&st, b, n-10)); }

    /* Zero-length data is a valid no-op. */
    memset(&st,0,sizeof st); g_data_bytes=0; run(&st, b, msg_begin(b));
    check("zero-length data ok",      run(&st, b, msg_data(b, 0)) && g_data_bytes==0);

    /* Builders produce the expected byte layout. */
    { uint8_t o[10]; size_t n=0;
      qemu_audio_build_set_format(o,&n, QA_FORMAT_S16, 2, 44100);
      check("set-format layout", n==10 && o[0]==255 && o[1]==1 && o[3]==QA_CLIENT_SET_FORMAT
            && o[4]==QA_FORMAT_S16 && o[5]==2
            && o[6]==0 && o[7]==0 && o[8]==0xAC && o[9]==0x44); /* 44100=0xAC44 */
    }

    printf("%s (%d failure%s)\n", failures?"FAILED":"PASSED", failures, failures==1?"":"s");
    return failures ? 1 : 0;
}
