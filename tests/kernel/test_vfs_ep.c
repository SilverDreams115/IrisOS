/*
 * test_vfs_ep.c — host unit tests for the VFS endpoint-protocol dispatcher
 * (services/vfs/vfs_ep.c, Fase 7.1).
 *
 * The dispatcher is a pure function, so every opcode and every validation
 * branch is exercised here without a kernel: valid LIST/STAT/READ_AT, PING,
 * unknown opcodes, short/long/non-NUL/undelivered payloads, word_count
 * violations, EOF and clamping semantics, visible-index skipping and
 * is_mapped (virt_base) reads.
 */
#include "framework.h"

#include <string.h>

#include <iris/nc/error.h>
#include <iris/endpoint_proto.h>
#include "../../services/vfs/vfs_ep.h"

/* ── Fixture ────────────────────────────────────────────────────────────── */

#define T_EXPORTS 4u

static struct vfs_export g_exp[T_EXPORTS];
static uint8_t g_mapped_backing[300];

static const char    t_iris_name[] = "iris.txt";
static const uint8_t t_iris_data[] = "Hello from IrisOS VFS!\n";
#define T_IRIS_LEN ((uint32_t)(sizeof(t_iris_data) - 1u))

static void t_setup_exports(void) {
    memset(g_exp, 0, sizeof(g_exp));

    /* [0] ready inline export */
    strcpy(g_exp[0].name, t_iris_name);
    memcpy(g_exp[0].data, t_iris_data, T_IRIS_LEN);
    g_exp[0].size  = T_IRIS_LEN;
    g_exp[0].ready = 1u;

    /* [1] NOT ready — must be invisible to LIST/STAT/READ_AT */
    strcpy(g_exp[1].name, "ghost.txt");
    g_exp[1].size  = 5u;
    g_exp[1].ready = 0u;

    /* [2] ready mapped export (reads via virt_base, larger than DATA_MAX) */
    for (uint32_t i = 0; i < (uint32_t)sizeof(g_mapped_backing); i++)
        g_mapped_backing[i] = (uint8_t)(i & 0xFFu);
    strcpy(g_exp[2].name, "big.bin");
    g_exp[2].size      = (uint32_t)sizeof(g_mapped_backing);
    g_exp[2].ready     = 1u;
    g_exp[2].is_mapped = 1u;
    g_exp[2].virt_base = (uint64_t)(uintptr_t)g_mapped_backing;

    /* [3] ready empty file */
    strcpy(g_exp[3].name, "empty.txt");
    g_exp[3].size  = 0u;
    g_exp[3].ready = 1u;
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

static struct IrisMsg g_req;
static struct IrisMsg g_reply;
static uint8_t        g_req_buf[VFS_EP_DATA_MAX];
static uint8_t        g_reply_buf[VFS_EP_DATA_MAX];

static void t_req_reset(uint64_t label) {
    memset(&g_req, 0, sizeof(g_req));
    memset(&g_reply, 0xAA, sizeof(g_reply));      /* detect unwritten reply */
    memset(g_reply_buf, 0xAA, sizeof(g_reply_buf));
    g_req.label = label;
}

static uint32_t t_stage_path(const char *path) {
    uint32_t n = (uint32_t)strlen(path);
    memcpy(g_req_buf, path, n + 1u);
    return n + 1u;
}

static void t_dispatch(void) {
    const uint8_t *rb = (g_req.buf_len > 0u) ? g_req_buf : NULL;
    vfs_ep_dispatch(g_exp, T_EXPORTS, &g_req, rb, &g_reply, g_reply_buf);
}

static void t_assert_err(iris_error_t err) {
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_ERR);
    ASSERT_EQ(g_reply.words[0], (uint64_t)(uint32_t)err);
    ASSERT_EQ(g_reply.word_count, 1u);
    ASSERT_EQ(g_reply.buf_len, 0u);
}

/* ── PING / unknown opcode ──────────────────────────────────────────────── */

static void t_ping(void) {
    t_req_reset(IRIS_EP_OP_PING);
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[0], 0u);
    ASSERT_EQ(g_reply.buf_len, 0u);
}

static void t_unknown_opcode(void) {
    t_req_reset(UINT64_C(0x0EEE));
    t_dispatch();
    t_assert_err(IRIS_ERR_NOT_SUPPORTED);

    /* SHUTDOWN is defined but not served by VFS */
    t_req_reset(IRIS_EP_OP_SHUTDOWN);
    t_dispatch();
    t_assert_err(IRIS_ERR_NOT_SUPPORTED);
}

/* ── LIST ───────────────────────────────────────────────────────────────── */

static void t_list_valid(void) {
    t_req_reset(VFS_EP_OP_LIST);
    g_req.words[0]   = 0;
    g_req.word_count = 1u;
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1], (uint64_t)T_IRIS_LEN);          /* size */
    ASSERT_EQ(g_reply.words[2], (uint64_t)strlen(t_iris_name)); /* name_len */
    ASSERT_EQ(g_reply.buf_len, (uint32_t)strlen(t_iris_name) + 1u);
    ASSERT_EQ(strcmp((const char *)g_reply_buf, t_iris_name), 0);
}

static void t_list_skips_not_ready(void) {
    /* visible index 1 must be exports[2] ("big.bin"), not the ghost */
    t_req_reset(VFS_EP_OP_LIST);
    g_req.words[0]   = 1;
    g_req.word_count = 1u;
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(strcmp((const char *)g_reply_buf, "big.bin"), 0);
}

static void t_list_out_of_range(void) {
    /* 3 ready exports → index 3 is past the end (end-of-listing) */
    t_req_reset(VFS_EP_OP_LIST);
    g_req.words[0]   = 3;
    g_req.word_count = 1u;
    t_dispatch();
    t_assert_err(IRIS_ERR_NOT_FOUND);

    t_req_reset(VFS_EP_OP_LIST);
    g_req.words[0]   = 999;
    g_req.word_count = 1u;
    t_dispatch();
    t_assert_err(IRIS_ERR_NOT_FOUND);
}

static void t_list_missing_words(void) {
    t_req_reset(VFS_EP_OP_LIST);
    g_req.word_count = 0u;
    t_dispatch();
    t_assert_err(IRIS_ERR_INVALID_ARG);
}

/* ── STAT ───────────────────────────────────────────────────────────────── */

static void t_stat_valid(void) {
    t_req_reset(VFS_EP_OP_STAT);
    g_req.buf_len = t_stage_path(t_iris_name);
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1], (uint64_t)T_IRIS_LEN);
    ASSERT_EQ(g_reply.buf_len, 0u);
}

static void t_stat_not_found(void) {
    t_req_reset(VFS_EP_OP_STAT);
    g_req.buf_len = t_stage_path("nope.txt");
    t_dispatch();
    t_assert_err(IRIS_ERR_NOT_FOUND);

    /* not-ready exports must be unreachable by name too */
    t_req_reset(VFS_EP_OP_STAT);
    g_req.buf_len = t_stage_path("ghost.txt");
    t_dispatch();
    t_assert_err(IRIS_ERR_NOT_FOUND);
}

static void t_stat_short_payload(void) {
    /* buf_len == 0 → no path at all */
    t_req_reset(VFS_EP_OP_STAT);
    g_req.buf_len = 0u;
    t_dispatch();
    t_assert_err(IRIS_ERR_INVALID_ARG);

    /* buf_len == 1 with leading NUL → empty name */
    t_req_reset(VFS_EP_OP_STAT);
    g_req_buf[0]  = 0u;
    g_req.buf_len = 1u;
    t_dispatch();
    t_assert_err(IRIS_ERR_INVALID_ARG);
}

static void t_stat_long_payload(void) {
    t_req_reset(VFS_EP_OP_STAT);
    memset(g_req_buf, 'a', VFS_EP_PATH_MAX);
    g_req_buf[VFS_EP_PATH_MAX] = 0u;
    g_req.buf_len = VFS_EP_PATH_MAX + 1u;   /* > PATH_MAX */
    t_dispatch();
    t_assert_err(IRIS_ERR_INVALID_ARG);
}

static void t_stat_not_nul_terminated(void) {
    t_req_reset(VFS_EP_OP_STAT);
    g_req.buf_len = t_stage_path(t_iris_name) - 1u; /* drop the NUL */
    t_dispatch();
    t_assert_err(IRIS_ERR_INVALID_ARG);
}

static void t_stat_undelivered_payload(void) {
    /* buf_len announces a payload but req_buf is NULL (delivery failed) */
    t_req_reset(VFS_EP_OP_STAT);
    g_req.buf_len = 8u;
    vfs_ep_dispatch(g_exp, T_EXPORTS, &g_req, NULL, &g_reply, g_reply_buf);
    t_assert_err(IRIS_ERR_INVALID_ARG);
}

/* ── READ_AT ────────────────────────────────────────────────────────────── */

static void t_read_at_valid(void) {
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.buf_len    = t_stage_path(t_iris_name);
    g_req.words[0]   = 0;
    g_req.words[1]   = VFS_EP_DATA_MAX;
    g_req.word_count = 2u;
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1], (uint64_t)T_IRIS_LEN);  /* bytes read */
    ASSERT_EQ(g_reply.words[2], (uint64_t)T_IRIS_LEN);  /* total size */
    ASSERT_EQ(g_reply.buf_len, T_IRIS_LEN);
    ASSERT_EQ(memcmp(g_reply_buf, t_iris_data, T_IRIS_LEN), 0);
}

static void t_read_at_offset(void) {
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.buf_len    = t_stage_path(t_iris_name);
    g_req.words[0]   = 6;                       /* "from IrisOS VFS!\n" */
    g_req.words[1]   = VFS_EP_DATA_MAX;
    g_req.word_count = 2u;
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1], (uint64_t)(T_IRIS_LEN - 6u));
    ASSERT_EQ(memcmp(g_reply_buf, t_iris_data + 6u, T_IRIS_LEN - 6u), 0);
}

static void t_read_at_eof(void) {
    /* offset == size → EOF, not an error */
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.buf_len    = t_stage_path(t_iris_name);
    g_req.words[0]   = T_IRIS_LEN;
    g_req.words[1]   = 16;
    g_req.word_count = 2u;
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1], 0u);
    ASSERT_EQ(g_reply.words[2], (uint64_t)T_IRIS_LEN);
    ASSERT_EQ(g_reply.buf_len, 0u);

    /* offset way past size → still EOF */
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.buf_len    = t_stage_path(t_iris_name);
    g_req.words[0]   = 10000;
    g_req.words[1]   = 16;
    g_req.word_count = 2u;
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1], 0u);

    /* empty file: offset 0 is already EOF */
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.buf_len    = t_stage_path("empty.txt");
    g_req.words[0]   = 0;
    g_req.words[1]   = 16;
    g_req.word_count = 2u;
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1], 0u);
    ASSERT_EQ(g_reply.words[2], 0u);
}

static void t_read_at_clamp_and_mapped(void) {
    /* mapped 300-byte export: request 1000 → clamped to VFS_EP_DATA_MAX */
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.buf_len    = t_stage_path("big.bin");
    g_req.words[0]   = 0;
    g_req.words[1]   = 1000;
    g_req.word_count = 2u;
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1], (uint64_t)VFS_EP_DATA_MAX);
    ASSERT_EQ(g_reply.words[2], (uint64_t)sizeof(g_mapped_backing));
    ASSERT_EQ(g_reply.buf_len, VFS_EP_DATA_MAX);
    ASSERT_EQ(memcmp(g_reply_buf, g_mapped_backing, VFS_EP_DATA_MAX), 0);

    /* tail read via virt_base: offset 256 → remaining 44 bytes */
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.buf_len    = t_stage_path("big.bin");
    g_req.words[0]   = VFS_EP_DATA_MAX;
    g_req.words[1]   = 1000;
    g_req.word_count = 2u;
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1],
              (uint64_t)(sizeof(g_mapped_backing) - VFS_EP_DATA_MAX));
    ASSERT_EQ(memcmp(g_reply_buf, g_mapped_backing + VFS_EP_DATA_MAX,
                     sizeof(g_mapped_backing) - VFS_EP_DATA_MAX), 0);
}

static void t_read_at_malformed(void) {
    /* word_count < 2 */
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.buf_len    = t_stage_path(t_iris_name);
    g_req.word_count = 1u;
    t_dispatch();
    t_assert_err(IRIS_ERR_INVALID_ARG);

    /* no path payload */
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.words[1]   = 16;
    g_req.word_count = 2u;
    g_req.buf_len    = 0u;
    t_dispatch();
    t_assert_err(IRIS_ERR_INVALID_ARG);

    /* non-NUL-terminated path */
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.buf_len    = t_stage_path(t_iris_name) - 1u;
    g_req.words[1]   = 16;
    g_req.word_count = 2u;
    t_dispatch();
    t_assert_err(IRIS_ERR_INVALID_ARG);

    /* oversized path */
    t_req_reset(VFS_EP_OP_READ_AT);
    memset(g_req_buf, 'a', VFS_EP_PATH_MAX);
    g_req_buf[VFS_EP_PATH_MAX] = 0u;
    g_req.buf_len    = VFS_EP_PATH_MAX + 1u;
    g_req.words[1]   = 16;
    g_req.word_count = 2u;
    t_dispatch();
    t_assert_err(IRIS_ERR_INVALID_ARG);

    /* unknown path */
    t_req_reset(VFS_EP_OP_READ_AT);
    g_req.buf_len    = t_stage_path("nope.txt");
    g_req.words[1]   = 16;
    g_req.word_count = 2u;
    t_dispatch();
    t_assert_err(IRIS_ERR_NOT_FOUND);
}

/* ── Defensive arguments ────────────────────────────────────────────────── */

/* ── STATUS (Fase 7.5) ──────────────────────────────────────────────────── */

static void t_status_valid(void) {
    t_req_reset(VFS_EP_OP_STATUS);
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[0], 0u);
    /* fixture: exports 0, 2, 3 are ready; 1 is not */
    ASSERT_EQ(g_reply.words[1], 3u);
    ASSERT_EQ(g_reply.words[2],
              (uint64_t)T_IRIS_LEN + (uint64_t)sizeof(g_mapped_backing));
    ASSERT_EQ(g_reply.word_count, 3u);
    ASSERT_EQ(g_reply.buf_len, 0u);
}

static void t_status_extra_words_ignored(void) {
    t_req_reset(VFS_EP_OP_STATUS);
    g_req.words[0]   = 0xDEADBEEFu;
    g_req.word_count = 4u;
    t_dispatch();
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1], 3u);
}

static void t_status_rejects_payload(void) {
    t_req_reset(VFS_EP_OP_STATUS);
    g_req.buf_len = t_stage_path("junk");
    t_dispatch();
    t_assert_err(IRIS_ERR_INVALID_ARG);
}

static void t_status_empty_table(void) {
    t_req_reset(VFS_EP_OP_STATUS);
    vfs_ep_dispatch(NULL, 0u, &g_req, NULL, &g_reply, g_reply_buf);
    ASSERT_EQ(g_reply.label, IRIS_EP_REPLY_OK);
    ASSERT_EQ(g_reply.words[1], 0u);
    ASSERT_EQ(g_reply.words[2], 0u);
}

static void t_defensive_args(void) {
    /* NULL request → error reply, never a crash */
    memset(&g_reply, 0xAA, sizeof(g_reply));
    vfs_ep_dispatch(g_exp, T_EXPORTS, NULL, NULL, &g_reply, g_reply_buf);
    t_assert_err(IRIS_ERR_INVALID_ARG);

    /* NULL reply_buf → error reply */
    t_req_reset(IRIS_EP_OP_PING);
    vfs_ep_dispatch(g_exp, T_EXPORTS, &g_req, NULL, &g_reply, NULL);
    t_assert_err(IRIS_ERR_INVALID_ARG);

    /* NULL exports with nonzero count → error reply */
    t_req_reset(IRIS_EP_OP_PING);
    vfs_ep_dispatch(NULL, T_EXPORTS, &g_req, NULL, &g_reply, g_reply_buf);
    t_assert_err(IRIS_ERR_INVALID_ARG);

    /* zero exports is a valid (empty) table: LIST 0 → NOT_FOUND */
    t_req_reset(VFS_EP_OP_LIST);
    g_req.words[0]   = 0;
    g_req.word_count = 1u;
    vfs_ep_dispatch(NULL, 0u, &g_req, NULL, &g_reply, g_reply_buf);
    t_assert_err(IRIS_ERR_NOT_FOUND);
}

/* ── Entry ──────────────────────────────────────────────────────────────── */

void test_vfs_ep(void) {
    TEST_SUITE("vfs_ep dispatcher (Fase 7.1)");
    t_setup_exports();

    t_ping();
    t_unknown_opcode();

    t_list_valid();
    t_list_skips_not_ready();
    t_list_out_of_range();
    t_list_missing_words();

    t_stat_valid();
    t_stat_not_found();
    t_stat_short_payload();
    t_stat_long_payload();
    t_stat_not_nul_terminated();
    t_stat_undelivered_payload();

    t_read_at_valid();
    t_read_at_offset();
    t_read_at_eof();
    t_read_at_clamp_and_mapped();
    t_read_at_malformed();

    t_status_valid();
    t_status_extra_words_ignored();
    t_status_rejects_payload();
    t_status_empty_table();

    t_defensive_args();
}
