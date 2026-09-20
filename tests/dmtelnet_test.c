#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmtelnet.h"

static dmtelnet_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmtelnet_create();
}

void dmod_test_teardown(void)
{
    dmtelnet_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmtelnet_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmtelnet_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmtelnet_is_valid(g_handle));
}

DMOD_TEST_STEP(dmtelnet_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmtelnet_destroy(NULL);
}
