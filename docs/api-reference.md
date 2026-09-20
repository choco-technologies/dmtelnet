# dmtelnet API Reference

dmtelnet is a transport-agnostic Telnet protocol engine (RFC 854). It never
touches a socket: it is fed raw bytes via `dmtelnet_recv()`, and produces
raw bytes via the `on_send` callback. See `include/dmtelnet.h` for the full
doc comments.

## Types

### `dmtelnet_t`

Opaque handle to one session, created by `dmtelnet_create()`.

### `dmtelnet_cmd_t`

RFC 855 negotiation commands: `dmtelnet_cmd_will`, `dmtelnet_cmd_wont`,
`dmtelnet_cmd_do`, `dmtelnet_cmd_dont`.

### `dmtelnet_callbacks_t`

```c
typedef struct
{
    dmtelnet_data_handler_t           on_data;
    dmtelnet_negotiate_handler_t      on_negotiate;
    dmtelnet_subnegotiation_handler_t on_subnegotiation;
    dmtelnet_send_handler_t           on_send;
} dmtelnet_callbacks_t;
```

`on_send` is required; every other member may be left `NULL` to ignore that
event.

### Option codes

`DMTELNET_OPT_BINARY`, `DMTELNET_OPT_ECHO`, `DMTELNET_OPT_SUPPRESS_GO_AHEAD`,
`DMTELNET_OPT_TERMINAL_TYPE`, `DMTELNET_OPT_NAWS`, `DMTELNET_OPT_LINEMODE`.

## Functions

| Function | Description |
|----------|-------------|
| `dmtelnet_create(callbacks, user_data)` | Create a session. Fails if `callbacks` or `callbacks->on_send` is `NULL`. |
| `dmtelnet_destroy(session)` | Destroy a session. Safe with `NULL`. |
| `dmtelnet_recv(session, data, data_len)` | Feed newly-received raw bytes; synchronously fires `on_data`/`on_negotiate`/`on_subnegotiation`. |
| `dmtelnet_send(session, data, data_len)` | Send application data (IAC-escaped) via `on_send`. |
| `dmtelnet_negotiate(session, cmd, option)` | Send `IAC <cmd> <option>`. |
| `dmtelnet_send_subnegotiation(session, option, data, data_len)` | Send `IAC SB <option> <data> IAC SE`. |

## Example: a minimal echo session over an already-connected transport

```c
#include "dmtelnet.h"

static void on_data(dmtelnet_t session, const uint8_t* data, size_t len, void* user_data)
{
    /* Echo whatever the peer typed straight back, through the same session
     * so the reply is correctly IAC-escaped again. */
    dmtelnet_send(session, data, len);
}

static void on_send(dmtelnet_t session, const uint8_t* data, size_t len, void* user_data)
{
    my_transport_write(user_data, data, len);
}

dmtelnet_callbacks_t callbacks = { .on_data = on_data, .on_send = on_send };
dmtelnet_t session = dmtelnet_create(&callbacks, my_transport_handle);

/* Whenever bytes arrive off the wire: */
dmtelnet_recv(session, wire_bytes, wire_len);
```

See `tools/telnetd` for a complete, real transport (dmtcp) built on this API.
