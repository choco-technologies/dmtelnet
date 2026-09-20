#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmtelnet.h"

#define CAPTURE_MAX 256

/* dmod modules have no libc memcpy()/memcmp() (see dmod/src/module/string.c's
 * minimal replacement set) - small manual loops stand in for them, same
 * convention as dmtcp/dmudp/dmicmp's own tests. */
static void bytes_copy(uint8_t* dst, const uint8_t* src, size_t len)
{
    for (size_t i = 0; i < len; i++)
        dst[i] = src[i];
}

static bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

static dmtelnet_t g_session = NULL;

/* on_data capture */
static uint8_t g_data_buf[CAPTURE_MAX];
static size_t g_data_len;
static int g_data_calls;

/* on_negotiate capture */
static dmtelnet_cmd_t g_neg_cmd;
static uint8_t g_neg_option;
static int g_neg_calls;

/* on_subnegotiation capture */
static uint8_t g_sub_buf[CAPTURE_MAX];
static size_t g_sub_len;
static uint8_t g_sub_option;
static int g_sub_calls;

/* on_send capture */
static uint8_t g_send_buf[CAPTURE_MAX];
static size_t g_send_len;

static void reset_captures(void)
{
    g_data_len = 0;
    g_data_calls = 0;
    g_neg_calls = 0;
    g_sub_len = 0;
    g_sub_calls = 0;
    g_send_len = 0;
}

static void on_data(dmtelnet_t session, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)session;
    (void)user_data;
    if (g_data_len + data_len <= sizeof(g_data_buf))
    {
        bytes_copy(g_data_buf + g_data_len, data, data_len);
        g_data_len += data_len;
    }
    g_data_calls++;
}

static void on_negotiate(dmtelnet_t session, dmtelnet_cmd_t cmd, uint8_t option, void* user_data)
{
    (void)session;
    (void)user_data;
    g_neg_cmd = cmd;
    g_neg_option = option;
    g_neg_calls++;
}

static void on_subnegotiation(dmtelnet_t session, uint8_t option, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)session;
    (void)user_data;
    g_sub_option = option;
    if (data_len <= sizeof(g_sub_buf))
    {
        bytes_copy(g_sub_buf, data, data_len);
        g_sub_len = data_len;
    }
    g_sub_calls++;
}

static void on_send(dmtelnet_t session, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)session;
    (void)user_data;
    if (g_send_len + data_len <= sizeof(g_send_buf))
    {
        bytes_copy(g_send_buf + g_send_len, data, data_len);
        g_send_len += data_len;
    }
}

void dmod_test_setup(void)
{
    reset_captures();

    dmtelnet_callbacks_t callbacks = {
        .on_data           = on_data,
        .on_negotiate      = on_negotiate,
        .on_subnegotiation = on_subnegotiation,
        .on_send           = on_send,
    };
    g_session = dmtelnet_create(&callbacks, NULL);
}

void dmod_test_teardown(void)
{
    dmtelnet_destroy(g_session);
    g_session = NULL;
}

DMOD_TEST_STEP(dmtelnet_create_requires_on_send)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_session);

    dmtelnet_callbacks_t no_send = { 0 };
    dmtelnet_t bad = dmtelnet_create(&no_send, NULL);
    DMOD_TEST_EXPECT_NULL(bad);

    DMOD_TEST_EXPECT_NULL(dmtelnet_create(NULL, NULL));
}

DMOD_TEST_STEP(dmtelnet_recv_plain_data)
{
    const uint8_t input[] = "hello";
    int ret = dmtelnet_recv(g_session, input, sizeof(input) - 1);

    DMOD_TEST_EXPECT_EQ(ret, 0);
    DMOD_TEST_EXPECT_EQ(g_data_calls, 1);
    DMOD_TEST_EXPECT_EQ(g_data_len, sizeof(input) - 1);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_data_buf, input, sizeof(input) - 1));
}

DMOD_TEST_STEP(dmtelnet_recv_data_around_negotiation)
{
    /* "ab" + IAC DO ECHO + "cd" - data either side of a negotiation command
     * must be reported (not dropped, not merged with the command bytes). */
    const uint8_t input[] = { 'a', 'b', DMTELNET_IAC, DMTELNET_DO, DMTELNET_OPT_ECHO, 'c', 'd' };
    dmtelnet_recv(g_session, input, sizeof(input));

    DMOD_TEST_EXPECT_EQ(g_neg_calls, 1);
    DMOD_TEST_EXPECT_EQ((int)g_neg_cmd, (int)dmtelnet_cmd_do);
    DMOD_TEST_EXPECT_EQ(g_neg_option, DMTELNET_OPT_ECHO);

    DMOD_TEST_EXPECT_EQ(g_data_len, sizeof("abcd") - 1);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_data_buf, (const uint8_t*)"abcd", 4));
}

DMOD_TEST_STEP(dmtelnet_recv_iac_escape)
{
    /* IAC IAC in the middle of data decodes to a single literal 0xFF byte. */
    const uint8_t input[] = { 'x', DMTELNET_IAC, DMTELNET_IAC, 'y' };
    dmtelnet_recv(g_session, input, sizeof(input));

    const uint8_t expected[] = { 'x', 0xFF, 'y' };
    DMOD_TEST_EXPECT_EQ(g_data_len, sizeof(expected));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_data_buf, expected, sizeof(expected)));
}

DMOD_TEST_STEP(dmtelnet_recv_subnegotiation)
{
    const uint8_t input[] = {
        DMTELNET_IAC, DMTELNET_SB, DMTELNET_OPT_NAWS, 0x00, 80, 0x00, 24,
        DMTELNET_IAC, DMTELNET_SE
    };
    dmtelnet_recv(g_session, input, sizeof(input));

    DMOD_TEST_EXPECT_EQ(g_sub_calls, 1);
    DMOD_TEST_EXPECT_EQ(g_sub_option, DMTELNET_OPT_NAWS);
    DMOD_TEST_EXPECT_EQ(g_sub_len, 4);

    const uint8_t expected[] = { 0x00, 80, 0x00, 24 };
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_sub_buf, expected, sizeof(expected)));
}

DMOD_TEST_STEP(dmtelnet_recv_subnegotiation_with_escaped_iac)
{
    /* Payload byte 0xFF inside a subnegotiation must also be escaped as
     * IAC IAC on the wire, and unescaped back to a single 0xFF on receive. */
    const uint8_t input[] = {
        DMTELNET_IAC, DMTELNET_SB, DMTELNET_OPT_TERMINAL_TYPE,
        0x00, 0xFF, 0xFF, 0x01,
        DMTELNET_IAC, DMTELNET_SE
    };
    dmtelnet_recv(g_session, input, sizeof(input));

    const uint8_t expected[] = { 0x00, 0xFF, 0x01 };
    DMOD_TEST_EXPECT_EQ(g_sub_calls, 1);
    DMOD_TEST_EXPECT_EQ(g_sub_len, sizeof(expected));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_sub_buf, expected, sizeof(expected)));
}

DMOD_TEST_STEP(dmtelnet_send_escapes_iac)
{
    const uint8_t input[] = { 'a', 0xFF, 'b' };
    int ret = dmtelnet_send(g_session, input, sizeof(input));

    const uint8_t expected[] = { 'a', DMTELNET_IAC, DMTELNET_IAC, 'b' };
    DMOD_TEST_EXPECT_EQ(ret, 0);
    DMOD_TEST_EXPECT_EQ(g_send_len, sizeof(expected));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_send_buf, expected, sizeof(expected)));
}

DMOD_TEST_STEP(dmtelnet_negotiate_sends_iac_cmd_option)
{
    int ret = dmtelnet_negotiate(g_session, dmtelnet_cmd_will, DMTELNET_OPT_SUPPRESS_GO_AHEAD);

    const uint8_t expected[] = { DMTELNET_IAC, DMTELNET_WILL, DMTELNET_OPT_SUPPRESS_GO_AHEAD };
    DMOD_TEST_EXPECT_EQ(ret, 0);
    DMOD_TEST_EXPECT_EQ(g_send_len, sizeof(expected));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_send_buf, expected, sizeof(expected)));
}

DMOD_TEST_STEP(dmtelnet_negotiate_rejects_bad_command)
{
    int ret = dmtelnet_negotiate(g_session, (dmtelnet_cmd_t)0, DMTELNET_OPT_ECHO);
    DMOD_TEST_EXPECT_EQ(ret, -22 /* EINVAL */);
}

DMOD_TEST_STEP(dmtelnet_subnegotiation_roundtrip)
{
    /* Sending a subnegotiation and feeding the exact bytes it produced back
     * into recv() must reproduce the original payload/option. */
    const uint8_t payload[] = { 'V', 'T', '1', '0', '0' };
    int ret = dmtelnet_send_subnegotiation(g_session, DMTELNET_OPT_TERMINAL_TYPE, payload, sizeof(payload));
    DMOD_TEST_EXPECT_EQ(ret, 0);

    uint8_t wire[CAPTURE_MAX];
    size_t wire_len = g_send_len;
    bytes_copy(wire, g_send_buf, wire_len);

    reset_captures();
    dmtelnet_recv(g_session, wire, wire_len);

    DMOD_TEST_EXPECT_EQ(g_sub_calls, 1);
    DMOD_TEST_EXPECT_EQ(g_sub_option, DMTELNET_OPT_TERMINAL_TYPE);
    DMOD_TEST_EXPECT_EQ(g_sub_len, sizeof(payload));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_sub_buf, payload, sizeof(payload)));
}

DMOD_TEST_STEP(dmtelnet_recv_rejects_null_data_with_nonzero_len)
{
    int ret = dmtelnet_recv(g_session, NULL, 5);
    DMOD_TEST_EXPECT_EQ(ret, -22 /* EINVAL */);
}

DMOD_TEST_STEP(dmtelnet_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmtelnet_destroy(NULL);
}
