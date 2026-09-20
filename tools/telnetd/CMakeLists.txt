# =====================================================================
#               telnetd - Telnet server (dmdrvi driver)
# =====================================================================
set(DMOD_MODULE_NAME        telnetd)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the DMR file for this module
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/telnetd.dmr)

dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/telnetd.c
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi
    dmini
    dmhaman
    dmtty
    dm_sw_ring
    dmosi
)

# dmtcp/dmip/dmroute/dmnetif: dmtelnet is this same repository's own
# top-level module, already built as part of this CMake configure by the
# time tools/ is added (see the root CMakeLists.txt) - link its
# dmtelnet_if target directly, exactly like dmtcp/tools/tcp_echo does for
# dmtcp_if, instead of fetching dmtelnet's own published headers back via
# dmod_link_modules() as if it were an external module.
target_link_libraries(${DMOD_MODULE_NAME} dmtelnet_if)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmtcp
    dmip
    dmroute
    dmnetif
)
