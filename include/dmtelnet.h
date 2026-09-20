#ifndef DMTELNET_H
#define DMTELNET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmtelnet_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * dmtelnet - a transport-agnostic Telnet protocol engine (RFC 854).
 *
 * dmtelnet never touches a socket itself: it is fed raw bytes as they
 * arrive off the wire via dmtelnet_recv(), and it hands raw bytes back out
 * via the on_send callback whenever something (application data via
 * dmtelnet_send(), or a negotiation/subnegotiation) needs to go out. This
 * is the same shape real libtelnet uses, and it means a session can sit on
 * top of dmtcp, dmudp, a UART, or a unit test's own loopback buffer without
 * this file knowing or caring which.
 *
 * IAC (0xFF) byte-stuffing (RFC 854 "when 255 is sent in the data stream,
 * it is necessary to double it") is handled transparently in both
 * directions: dmtelnet_recv() reports a doubled 0xFF as a single literal
 * data byte, and dmtelnet_send() doubles any literal 0xFF before handing
 * data to on_send. Callers of dmtelnet_data_handler_t / dmtelnet_send()
 * never need to think about escaping.
 *
 * Only the wire protocol lives here - policy (which options to offer or
 * agree to, what to do with a subnegotiation payload) belongs to the
 * caller, driven by on_negotiate/on_subnegotiation.
 */

/* Opaque handle - the real struct is defined in src/dmtelnet.c */
typedef struct dmtelnet* dmtelnet_t;

/**
 * Maximum number of payload bytes buffered for one subnegotiation
 * (IAC SB <option> ... IAC SE). A payload longer than this is silently
 * truncated - generous for the common cases (NAWS is 4 bytes, TERMINAL-TYPE
 * replies are a handful of ASCII characters).
 */
#define DMTELNET_MAX_SUBNEGOTIATION_LEN 128u

/* RFC 854 command bytes */
#define DMTELNET_IAC  255u  /**< Interpret As Command */
#define DMTELNET_SE   240u  /**< End of subnegotiation parameters */
#define DMTELNET_GA   249u  /**< Go ahead */
#define DMTELNET_SB   250u  /**< Start of subnegotiation parameters */
#define DMTELNET_WILL 251u
#define DMTELNET_WONT 252u
#define DMTELNET_DO   253u
#define DMTELNET_DONT 254u

/** RFC 854/855 negotiation commands, reported via dmtelnet_negotiate_handler_t */
typedef enum
{
    dmtelnet_cmd_will = DMTELNET_WILL, /**< Sender wants to enable an option */
    dmtelnet_cmd_wont = DMTELNET_WONT, /**< Sender refuses/disables an option */
    dmtelnet_cmd_do   = DMTELNET_DO,   /**< Sender asks the peer to enable an option */
    dmtelnet_cmd_dont = DMTELNET_DONT, /**< Sender asks the peer to disable an option */
} dmtelnet_cmd_t;

/* A handful of common option codes (RFC 856/857/1073/1091/1184/...) - not an
 * exhaustive list, just enough that a caller doesn't have to look up the
 * RFCs for the options most servers/clients actually negotiate. */
#define DMTELNET_OPT_BINARY             0u  /**< RFC 856 */
#define DMTELNET_OPT_ECHO               1u  /**< RFC 857 */
#define DMTELNET_OPT_SUPPRESS_GO_AHEAD  3u  /**< RFC 858 */
#define DMTELNET_OPT_TERMINAL_TYPE      24u /**< RFC 1091 */
#define DMTELNET_OPT_NAWS               31u /**< RFC 1073 - window size */
#define DMTELNET_OPT_LINEMODE           34u /**< RFC 1184 */

/**
 * In-band application data, decoded (IAC-escaping removed, negotiation and
 * subnegotiation bytes stripped out). `data`/`data_len` are borrowed, valid
 * only for the duration of the call - copy out anything to keep.
 *
 * May be called zero or more times from within a single dmtelnet_recv()
 * call, and never with data_len == 0.
 */
typedef void (*dmtelnet_data_handler_t)(dmtelnet_t session, const uint8_t* data, size_t data_len, void* user_data);

/**
 * Fires once per negotiation command received: IAC WILL/WONT/DO/DONT <option>.
 * Purely informational - dmtelnet applies no policy of its own (e.g.
 * receiving DO ECHO does not make dmtelnet start echoing anything). Reply
 * with dmtelnet_negotiate() from inside this callback if a reply is needed.
 */
typedef void (*dmtelnet_negotiate_handler_t)(dmtelnet_t session, dmtelnet_cmd_t cmd, uint8_t option, void* user_data);

/**
 * Fires once a full subnegotiation has been received: IAC SB <option> ...
 * IAC SE. `data`/`data_len` are borrowed, valid only for the duration of
 * the call, and describe the payload between <option> and the closing
 * IAC SE (already IAC-unescaped). Truncated to DMTELNET_MAX_SUBNEGOTIATION_LEN
 * if the peer sent more than that.
 */
typedef void (*dmtelnet_subnegotiation_handler_t)(dmtelnet_t session, uint8_t option, const uint8_t* data, size_t data_len, void* user_data);

/**
 * The only way dmtelnet ever produces wire bytes - every one of
 * dmtelnet_send()/_negotiate()/_send_subnegotiation() ends up calling this,
 * once or more per call. `data`/`data_len` are borrowed, valid only for the
 * duration of the call (e.g. hand them straight to dmtcp_send()).
 *
 * Required: dmtelnet_create() fails if this is NULL, since a session with
 * no way to emit bytes cannot do anything useful.
 */
typedef void (*dmtelnet_send_handler_t)(dmtelnet_t session, const uint8_t* data, size_t data_len, void* user_data);

/**
 * Callbacks for one session. Any member other than on_send may be left
 * NULL, in which case the corresponding event is simply dropped.
 */
typedef struct
{
    dmtelnet_data_handler_t           on_data;
    dmtelnet_negotiate_handler_t      on_negotiate;
    dmtelnet_subnegotiation_handler_t on_subnegotiation;
    dmtelnet_send_handler_t           on_send;
} dmtelnet_callbacks_t;

/**
 * Create a new Telnet session.
 *
 * @param callbacks Copied - the pointer need not outlive this call.
 *                  callbacks->on_send must be non-NULL.
 * @param user_data Opaque pointer passed back to every callback.
 *
 * @return A valid handle on success, or NULL if callbacks is NULL,
 *         callbacks->on_send is NULL, or on allocation failure.
 */
dmod_dmtelnet_api(1.0, dmtelnet_t, _create, ( const dmtelnet_callbacks_t* callbacks, void* user_data ));

/**
 * Destroy a session created by dmtelnet_create(). Safe to call with NULL.
 */
dmod_dmtelnet_api(1.0, void, _destroy, ( dmtelnet_t session ));

/**
 * Feed newly-received raw bytes into the session. Synchronously invokes
 * on_data/on_negotiate/on_subnegotiation zero or more times before
 * returning, in the order the corresponding protocol elements appeared in
 * `data`. May itself call on_send if a malformed subnegotiation forces the
 * parser to resynchronize - never as a reply to ordinary data.
 *
 * @return 0 on success, -EINVAL if `session` is NULL, or if `data` is NULL
 *         while `data_len` is nonzero.
 */
dmod_dmtelnet_api(1.0, int, _recv, ( dmtelnet_t session, const uint8_t* data, size_t data_len ));

/**
 * Send application data: escapes any literal 0xFF byte (IAC IAC) and hands
 * the result to on_send, in one or more calls.
 *
 * @return 0 on success, -EINVAL if `session` is NULL, or if `data` is NULL
 *         while `data_len` is nonzero.
 */
dmod_dmtelnet_api(1.0, int, _send, ( dmtelnet_t session, const uint8_t* data, size_t data_len ));

/**
 * Send a negotiation command: IAC <cmd> <option>.
 *
 * @return 0 on success, -EINVAL if `session` is NULL or `cmd` is not one of
 *         dmtelnet_cmd_will/_wont/_do/_dont.
 */
dmod_dmtelnet_api(1.0, int, _negotiate, ( dmtelnet_t session, dmtelnet_cmd_t cmd, uint8_t option ));

/**
 * Send a subnegotiation: IAC SB <option> <data, IAC-escaped> IAC SE.
 *
 * @return 0 on success, -EINVAL if `session` is NULL, or if `data` is NULL
 *         while `data_len` is nonzero.
 */
dmod_dmtelnet_api(1.0, int, _send_subnegotiation, ( dmtelnet_t session, uint8_t option, const uint8_t* data, size_t data_len ));

#ifdef __cplusplus
}
#endif

#endif // DMTELNET_H
