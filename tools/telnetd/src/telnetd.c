/**
 * telnetd - a dmdrvi driver that turns each accepted TCP connection on its
 * listening port into a dmtty-compatible device node, so that the existing
 * console@.ini/console.rules machinery (see dmtty/tools/console) starts a
 * dmell login shell on it automatically, exactly as it would for a physical
 * UART. telnetd itself never spawns a shell or touches dmtty/dmell
 * directly - it only speaks dmdrvi (to dmdevfs) and dmhaman (to announce a
 * new tty-class device), the same indirection dmuart uses so a driver never
 * needs a hard dependency on the terminal stack sitting on top of it.
 *
 * Per-connection data flow:
 *
 *   dmtcp (TCP bytes) <-> dmtelnet (Telnet protocol) <-> dm_sw_ring (RX) <-> dmdrvi _read/_write <-> dmdevfs/dmvfs <-> dmtty <-> dmell
 *
 * dm_sw_ring decouples dmtcp's asynchronous, callback-driven delivery from
 * dmdrvi_read()'s expected semantics (block until at least one byte is
 * available) - see telnetd_dmdrvi_read()'s own comment for why this is a
 * polling loop rather than the ring's own blocking wait, which has no way
 * to be woken up by a peer disconnect.
 *
 * Known limitations (deliberately out of scope for this first version):
 *   - dmtcp_send() is best-effort: a short write (outbound buffer full) is
 *     silently dropped rather than retried via dmtcp_writable_handler_t.
 *     Fine for interactive terminal traffic; would need real backpressure
 *     plumbed all the way to dmtty for bulk transfers.
 *   - A dmell session ending does not itself hang up the TCP connection
 *     (console@.ini leaves the tty node attached, waiting for a new
 *     session - see its own doc comment). Only the peer disconnecting (or
 *     the tty node being explicitly detached) closes the connection.
 *
 * NVT ASCII line endings (RFC 854): the Telnet protocol's "network virtual
 * terminal" requires a CR LF (or CR NUL) pair for end-of-line, while dmtty's
 * own line discipline (like the rest of this codebase) is plain Unix '\n'.
 * telnetd translates between the two at its own boundary - see
 * telnet_on_data()'s inbound normalization and telnetd_dmdrvi_write()'s
 * outbound expansion - so nothing above it (dm_sw_ring, dmtty, dmell) ever
 * has to know Telnet uses a different convention.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmdrvi.h"
#include "dmini.h"
#include "dmhaman.h"
#include "dmtty_types.h"
#include "dmtcp.h"
#include "dmtelnet.h"
#include "dm_sw_ring.h"
#include "dmosi.h"
#include <errno.h>
#include <string.h>

#define TELNETD_CONTEXT_MAGIC     0x544E4C44u /* 'TNLD' */
#define TELNETD_DEFAULT_PORT      23u
#define TELNETD_MAX_CONNECTIONS   4u
#define TELNETD_RX_RING_CAPACITY  512u
#define TELNETD_READ_POLL_MS      50u

typedef struct
{
    bool in_use;   /**< Slot holds a live (or tearing-down) connection */
    bool opened;   /**< telnetd_dmdrvi_open() has been called for this slot - see teardown_connection() */
    bool closed;   /**< The TCP side is gone; telnetd_dmdrvi_read() should report EOF */

    dmdrvi_dev_num_t dev_num;
    char* path;             /**< Set once telnetd_dmdrvi_path_ready() fires */

    dmtcp_conn_t conn;       /**< NULL once the connection has ended - see dmtcp_conn_t's "Handle lifetime" doc comment */
    dmtelnet_t telnet;
    dm_sw_ring_t rx_ring;    /**< Decoded (non-command) bytes, already CRLF-normalized to '\n', waiting to be read */
    bool rx_pending_cr;      /**< See telnet_on_data(): a CR was seen and might still be the start of a CR LF/CR NUL pair */

    struct dmdrvi_context* context;
} telnetd_connection_t;

struct dmdrvi_context
{
    uint32_t magic;
    uint16_t port;
    dmdrvi_dev_id_t next_minor;
    dmosi_mutex_t connections_mutex; /**< Guards the array below: populated from the dmtcp accept thread, read from dmdevfs's own worker thread */
    telnetd_connection_t connections[TELNETD_MAX_CONNECTIONS];
};

/* dmdevfs only ever creates one telnetd context (one [main] driver_name=telnetd
 * config entry - see configs/telnetd.ini) - kept so telnetd_on_accept(), which
 * dmtcp_listen() gives no way to pass a user_data pointer to, can reach it. */
static struct dmdrvi_context* g_context = NULL;

static bool is_valid_context(dmdrvi_context_t context)
{
    return context != NULL && context->magic == TELNETD_CONTEXT_MAGIC;
}

static telnetd_connection_t* find_connection_by_minor_locked(struct dmdrvi_context* context, dmdrvi_dev_id_t minor)
{
    for (size_t i = 0; i < TELNETD_MAX_CONNECTIONS; i++)
    {
        if (context->connections[i].in_use && context->connections[i].dev_num.minor == minor)
            return &context->connections[i];
    }
    return NULL;
}

static telnetd_connection_t* find_free_connection_slot_locked(struct dmdrvi_context* context)
{
    for (size_t i = 0; i < TELNETD_MAX_CONNECTIONS; i++)
    {
        if (!context->connections[i].in_use)
            return &context->connections[i];
    }
    return NULL;
}

/* ============================================================================
 *                      dmtelnet callbacks
 * ========================================================================== */

/**
 * NVT ASCII (RFC 854) sends CR LF, or CR NUL, for what dmtty/dmell just
 * want as a single '\n' - and, per the RFC, a bare CR with neither is still
 * a valid end-of-line, not a literal carriage return. Normalizes all three
 * to '\n' in place, byte by byte, using c->rx_pending_cr to remember a CR
 * across calls in case the LF/NUL half of the pair arrives in the next TCP
 * segment.
 */
static void telnet_on_data(dmtelnet_t session, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)session;
    telnetd_connection_t* c = user_data;

    uint8_t normalized[64];
    size_t normalized_len = 0;

    for (size_t i = 0; i < data_len; i++)
    {
        uint8_t byte = data[i];

        if (c->rx_pending_cr)
        {
            c->rx_pending_cr = false;
            if (byte == '\n' || byte == '\0')
                continue; /* Second half of a CR LF / CR NUL pair - already emitted the '\n' for the CR itself */
        }

        if (byte == '\r')
        {
            c->rx_pending_cr = true;
            byte = '\n';
        }

        if (normalized_len >= sizeof(normalized))
        {
            dm_sw_ring_write(c->rx_ring, normalized, (dm_sw_ring_capacity_t)normalized_len);
            normalized_len = 0;
        }
        normalized[normalized_len++] = byte;
    }

    if (normalized_len > 0)
        dm_sw_ring_write(c->rx_ring, normalized, (dm_sw_ring_capacity_t)normalized_len);
}

static void telnet_on_send(dmtelnet_t session, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)session;
    telnetd_connection_t* c = user_data;
    if (c->conn != NULL)
    {
        /* Best-effort - see this file's top comment. */
        dmtcp_send(c->conn, data, data_len);
    }
}

/* dmtelnet applies no negotiation policy of its own - reply here with the
 * minimum needed for a sane interactive line-mode session: we do the
 * echoing (dmtty already echoes at the tty layer, see dmtty_flag_echo) and
 * neither side waits for a go-ahead. Everything else offered is refused. */
static void telnet_on_negotiate(dmtelnet_t session, dmtelnet_cmd_t cmd, uint8_t option, void* user_data)
{
    (void)user_data;
    switch (cmd)
    {
    case dmtelnet_cmd_do:
        if (option == DMTELNET_OPT_ECHO || option == DMTELNET_OPT_SUPPRESS_GO_AHEAD)
            dmtelnet_negotiate(session, dmtelnet_cmd_will, option);
        else
            dmtelnet_negotiate(session, dmtelnet_cmd_wont, option);
        break;
    case dmtelnet_cmd_will:
        if (option == DMTELNET_OPT_SUPPRESS_GO_AHEAD)
            dmtelnet_negotiate(session, dmtelnet_cmd_do, option);
        else
            dmtelnet_negotiate(session, dmtelnet_cmd_dont, option);
        break;
    default:
        /* WONT/DONT are acknowledgements, not requests - no reply needed. */
        break;
    }
}

/* ============================================================================
 *                      dmtcp callbacks
 * ========================================================================== */

/**
 * Unwinds a connection: announces it away from dmtty/dmdevfs, then either
 * frees local resources immediately (nothing ever opened this device's
 * backing file, so nothing else can be touching it) or leaves that to
 * telnetd_dmdrvi_close(), which dmtty's own detach - triggered
 * synchronously by the DEVICE_UNAVAILABLE broadcast below - is guaranteed
 * to call before this function returns.
 *
 * Called from dmtcp's own terminal callbacks (on_closed/on_reset/on_error),
 * where `conn` is about to be freed - never touches `c->conn` itself, per
 * dmtcp_conn_t's "Handle lifetime" doc comment.
 */
static void teardown_connection(telnetd_connection_t* c)
{
    if (c == NULL || !c->in_use || c->closed)
        return;

    c->closed = true;
    c->conn = NULL;

    if (c->path != NULL)
    {
        dmtty_device_unavailable_params_t tty_params = { .path = c->path };
        dmhaman_call_handler(DMTTY_HANDLER_NAME_DEVICE_UNAVAILABLE, &tty_params);
    }

    dmdrvi_device_unavailable(c->context, &c->dev_num);

    if (!c->opened)
    {
        /* No dmdrvi_open() ever happened for this slot (e.g. the peer
         * dropped the connection before dmtty got around to attaching it) -
         * telnetd_dmdrvi_close() will never be called for it, so free here
         * instead of leaking the slot forever. */
        if (c->telnet != NULL) { dmtelnet_destroy(c->telnet); c->telnet = NULL; }
        if (c->rx_ring != NULL) { dm_sw_ring_destroy(c->rx_ring); c->rx_ring = NULL; }
        Dmod_Free(c->path);
        c->path = NULL;
        c->in_use = false;
    }
}

static void tcp_on_data(dmtcp_conn_t conn, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)conn;
    telnetd_connection_t* c = user_data;
    if (data == NULL)
    {
        /* Peer's FIN (dmtcp_data_handler_t's read()-returns-0 convention) -
         * no wire representation for this in Telnet, nothing to relay. */
        return;
    }
    dmtelnet_recv(c->telnet, data, data_len);
}

static void tcp_on_closed(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    teardown_connection((telnetd_connection_t*)user_data);
}

static void tcp_on_reset(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    teardown_connection((telnetd_connection_t*)user_data);
}

static void tcp_on_error(dmtcp_conn_t conn, int error, void* user_data)
{
    (void)conn;
    (void)error;
    teardown_connection((telnetd_connection_t*)user_data);
}

static void telnetd_on_accept(dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    (void)peer;
    (void)peer_port;
    (void)iface;

    struct dmdrvi_context* context = g_context;
    if (context == NULL)
    {
        dmtcp_abort(conn);
        return;
    }

    dmosi_mutex_lock(context->connections_mutex);
    telnetd_connection_t* c = find_free_connection_slot_locked(context);
    if (c != NULL)
    {
        memset(c, 0, sizeof(*c));
        c->in_use = true;
    }
    dmosi_mutex_unlock(context->connections_mutex);

    if (c == NULL)
    {
        DMOD_LOG_WARN("telnetd: too many connections (max %u), rejecting\n", (unsigned)TELNETD_MAX_CONNECTIONS);
        dmtcp_abort(conn);
        return;
    }

    c->rx_ring = dm_sw_ring_create(TELNETD_RX_RING_CAPACITY, dm_sw_ring_flags_mutex_sync | dm_sw_ring_flags_drop_old_data);
    if (c->rx_ring == NULL)
    {
        c->in_use = false;
        dmtcp_abort(conn);
        return;
    }

    dmtelnet_callbacks_t telnet_callbacks = {
        .on_data      = telnet_on_data,
        .on_negotiate = telnet_on_negotiate,
        .on_send      = telnet_on_send,
    };
    c->telnet = dmtelnet_create(&telnet_callbacks, c);
    if (c->telnet == NULL)
    {
        dm_sw_ring_destroy(c->rx_ring);
        c->rx_ring = NULL;
        c->in_use = false;
        dmtcp_abort(conn);
        return;
    }

    c->conn = conn;
    c->context = context;

    dmosi_mutex_lock(context->connections_mutex);
    c->dev_num.flags = DMDRVI_NUM_MINOR;
    c->dev_num.major = 0;
    c->dev_num.minor = context->next_minor++;
    dmosi_mutex_unlock(context->connections_mutex);

    dmtcp_conn_callbacks_t tcp_callbacks = {
        .on_data   = tcp_on_data,
        .on_closed = tcp_on_closed,
        .on_reset  = tcp_on_reset,
        .on_error  = tcp_on_error,
    };
    dmtcp_conn_set_callbacks(conn, &tcp_callbacks, c);

    dmtelnet_negotiate(c->telnet, dmtelnet_cmd_will, DMTELNET_OPT_ECHO);
    dmtelnet_negotiate(c->telnet, dmtelnet_cmd_will, DMTELNET_OPT_SUPPRESS_GO_AHEAD);

    /* Announces the new device to dmdevfs, which assigns it a path and
     * reports it back via telnetd_dmdrvi_path_ready() - that is what
     * actually creates the tty node (see that function's doc comment). */
    dmdrvi_device_available(context, &c->dev_num);
}

/* ============================================================================
 *                      DMDRVI interface
 * ========================================================================== */

dmod_dmdrvi_dif_api_declaration(2.0, telnetd, dmdrvi_context_t, _create, ( dmini_context_t config, dmdrvi_dev_num_t* dev_num ))
{
    if (dev_num == NULL)
    {
        DMOD_LOG_ERROR("Invalid parameters to telnetd_dmdrvi_create\n");
        return NULL;
    }

    struct dmdrvi_context* context = Dmod_Malloc(sizeof(*context));
    if (context == NULL)
        return NULL;

    memset(context, 0, sizeof(*context));
    context->magic = TELNETD_CONTEXT_MAGIC;
    context->port = (uint16_t)dmini_get_int(config, "main", "port", (int)TELNETD_DEFAULT_PORT);
    context->next_minor = 1; /* 0 is reserved for the placeholder device below */

    context->connections_mutex = dmosi_mutex_create(false);
    if (context->connections_mutex == NULL)
    {
        Dmod_Free(context);
        return NULL;
    }

    int ret = dmtcp_listen(context->port, telnetd_on_accept);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("telnetd: failed to listen on TCP port %u (error %d)\n", (unsigned)context->port, ret);
        dmosi_mutex_destroy(context->connections_mutex);
        Dmod_Free(context);
        return NULL;
    }

    g_context = context;

    /* No static device - every real device is created dynamically, one per
     * accepted connection, via dmdrvi_device_available() in
     * telnetd_on_accept(). This dev_num describes an inert placeholder
     * (nothing ever calls telnetd_dmdrvi_open() for minor 0 successfully),
     * matching dmdevfs's own "no device numbers" example for a driver with
     * nothing static to expose. */
    dev_num->flags = DMDRVI_NUM_NONE;
    dev_num->major = 0;
    dev_num->minor = 0;

    DMOD_LOG_INFO("telnetd: listening on TCP port %u\n", (unsigned)context->port);
    return context;
}

dmod_dmdrvi_dif_api_declaration(2.0, telnetd, void, _path_ready, ( dmdrvi_context_t context, const dmdrvi_dev_num_t* dev_num, const char* path ))
{
    if (!is_valid_context(context) || dev_num == NULL || path == NULL)
        return;

    dmosi_mutex_lock(context->connections_mutex);
    telnetd_connection_t* c = find_connection_by_minor_locked(context, dev_num->minor);
    dmosi_mutex_unlock(context->connections_mutex);
    if (c == NULL)
        return; /* The placeholder device (minor 0), or a connection that already ended */

    c->path = Dmod_StrDup(path);
    if (c->path == NULL)
        return;

    /* Announce this device to dmtty via dmhaman, without linking directly
     * against it, mirroring dmuart's own _path_ready() - see this file's
     * top comment for the full data-flow this plugs into. */
    dmtty_device_available_params_t tty_params = {
        .path  = c->path,
        .name  = NULL,
        .flags = 0,
    };
    dmhaman_call_handler(DMTTY_HANDLER_NAME_DEVICE_AVAILABLE, &tty_params);
}

dmod_dmdrvi_dif_api_declaration(2.0, telnetd, void, _free, ( dmdrvi_context_t context ))
{
    if (!is_valid_context(context))
        return;

    dmtcp_unlisten(context->port);

    /* The whole driver is going away - no graceful per-connection
     * negotiation (that requires the dmtcp terminal-callback restrictions
     * teardown_connection() itself documents), just release what is local
     * to this module. */
    for (size_t i = 0; i < TELNETD_MAX_CONNECTIONS; i++)
    {
        telnetd_connection_t* c = &context->connections[i];
        if (!c->in_use)
            continue;

        if (c->telnet != NULL) dmtelnet_destroy(c->telnet);
        if (c->rx_ring != NULL) dm_sw_ring_destroy(c->rx_ring);
        Dmod_Free(c->path);
    }

    dmosi_mutex_destroy(context->connections_mutex);

    if (g_context == context)
        g_context = NULL;

    context->magic = 0;
    Dmod_Free(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, telnetd, void*, _open, ( dmdrvi_context_t context, int flags, const dmdrvi_dev_num_t* dev_num ))
{
    (void)flags;
    if (!is_valid_context(context) || dev_num == NULL)
        return NULL;

    dmosi_mutex_lock(context->connections_mutex);
    telnetd_connection_t* c = find_connection_by_minor_locked(context, dev_num->minor);
    if (c != NULL)
        c->opened = true;
    dmosi_mutex_unlock(context->connections_mutex);

    return c;
}

dmod_dmdrvi_dif_api_declaration(2.0, telnetd, void, _close, ( dmdrvi_context_t context, void* handle ))
{
    (void)context;
    telnetd_connection_t* c = handle;
    if (c == NULL)
        return;

    if (c->conn != NULL)
    {
        /* A local close (tty node detached by hand, or a future
         * service-stop) rather than a peer disconnect - hang up gracefully.
         * Safe to touch c->conn here: unlike teardown_connection(), this is
         * never called from inside one of dmtcp's own terminal callbacks. */
        dmtcp_close(c->conn);
        c->conn = NULL;
    }

    if (c->telnet != NULL) { dmtelnet_destroy(c->telnet); c->telnet = NULL; }
    if (c->rx_ring != NULL) { dm_sw_ring_destroy(c->rx_ring); c->rx_ring = NULL; }
    Dmod_Free(c->path);
    c->path = NULL;
    c->in_use = false;
}

/**
 * Blocks until at least one byte is available or the connection has ended.
 *
 * A plain dm_sw_ring_read() with dm_sw_ring_flags_wait_for_some_data would
 * do this too, but with no way to wake it up on a peer disconnect - the
 * writer side (telnet_on_data()) simply never fires again, and the ring has
 * no "close" concept of its own to unblock a waiting reader. Polling this
 * loop's own `c->closed` flag (set by teardown_connection(), which runs on
 * a different thread) is the tradeoff: bounded by TELNETD_READ_POLL_MS
 * latency on a disconnect, instead of a blocked reader thread forever.
 *
 * @param context DMDRVI context (unused - the connection is non-seekable and
 * stream-oriented, so there is nothing context-level to look up)
 * @param handle Device handle (the connection slot)
 * @param buffer Buffer to read data into
 * @param size Number of bytes requested; values greater than INT64_MAX fail
 * with -EOVERFLOW because they cannot be represented by dmdrvi_ssize_t
 * @param offset Unused - telnetd is a non-seekable stream device; must still
 * be non-negative per the dmdrvi 2.0 contract
 *
 * @return dmdrvi_ssize_t Number of bytes read, zero at EOF (peer
 * disconnected), or a negative errno-compatible error
 */
dmod_dmdrvi_dif_api_declaration(2.0, telnetd, dmdrvi_ssize_t, _read, ( dmdrvi_context_t context, void* handle, void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    (void)context;
    (void)offset;

    if (offset < 0)
        return -EINVAL;
    if (size > (size_t)INT64_MAX)
        return -EOVERFLOW;

    telnetd_connection_t* c = handle;
    if (c == NULL || buffer == NULL || size == 0)
        return 0;

    for (;;)
    {
        dm_sw_ring_capacity_t got = dm_sw_ring_read(c->rx_ring, buffer, (dm_sw_ring_capacity_t)size);
        if (got > 0)
            return (dmdrvi_ssize_t)got;
        if (c->closed)
            return 0; /* EOF */
        dmosi_thread_sleep(TELNETD_READ_POLL_MS);
    }
}

/**
 * @param context DMDRVI context (unused, see telnetd_dmdrvi_read())
 * @param handle Device handle (the connection slot)
 * @param buffer Buffer with data to write
 * @param size Number of bytes to write; values greater than INT64_MAX fail
 * with -EOVERFLOW because they cannot be represented by dmdrvi_ssize_t
 * @param offset Unused - telnetd is a non-seekable stream device; must still
 * be non-negative per the dmdrvi 2.0 contract
 *
 * @return dmdrvi_ssize_t Number of bytes written (always `size` on success,
 * since this is best-effort per this file's top comment), or a negative
 * errno-compatible error
 */
dmod_dmdrvi_dif_api_declaration(2.0, telnetd, dmdrvi_ssize_t, _write, ( dmdrvi_context_t context, void* handle, const void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    (void)context;
    (void)offset;

    if (offset < 0)
        return -EINVAL;
    if (size > (size_t)INT64_MAX)
        return -EOVERFLOW;

    telnetd_connection_t* c = handle;
    if (c == NULL || c->closed || buffer == NULL || size == 0)
        return 0;

    /* Expand dmtty's plain '\n' to NVT ASCII's required CR LF before
     * IAC-escaping and sending - see this file's top comment. Flushed in
     * bounded chunks through dmtelnet_send(), which hands each one to
     * telnet_on_send() (the actual dmtcp_send(), best-effort). */
    const uint8_t* bytes = buffer;
    uint8_t expanded[64];
    size_t expanded_len = 0;

    for (size_t i = 0; i < size; i++)
    {
        if (expanded_len >= sizeof(expanded) - 1)
        {
            dmtelnet_send(c->telnet, expanded, expanded_len);
            expanded_len = 0;
        }

        if (bytes[i] == '\n')
            expanded[expanded_len++] = '\r';
        expanded[expanded_len++] = bytes[i];
    }

    if (expanded_len > 0)
        dmtelnet_send(c->telnet, expanded, expanded_len);

    return (dmdrvi_ssize_t)size;
}

dmod_dmdrvi_dif_api_declaration(2.0, telnetd, int, _ioctl, ( dmdrvi_context_t context, void* handle, int command, void* arg ))
{
    (void)context;
    (void)handle;
    (void)command;
    (void)arg;
    return -ENOSYS;
}

dmod_dmdrvi_dif_api_declaration(2.0, telnetd, int, _flush, ( dmdrvi_context_t context, void* handle ))
{
    (void)context;
    (void)handle;
    return 0; /* Nothing buffered locally beyond what dmtcp already owns. */
}

dmod_dmdrvi_dif_api_declaration(2.0, telnetd, int, _stat, ( dmdrvi_context_t context, const char* path, dmdrvi_stat_t* stat ))
{
    (void)context;
    (void)path;
    if (stat == NULL)
        return -EINVAL;

    stat->size = 0;   /* Stream device, no fixed size */
    stat->mode = 0666;
    return 0;
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;
    return 0;
}

int dmod_deinit(void)
{
    return 0;
}
