#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmtelnet.h"
#include <errno.h>
#include <string.h>

/**
 * Parser state machine, driven byte-by-byte by dmtelnet_recv(). Plain data
 * runs are reported in whole contiguous chunks (not byte-at-a-time) -
 * `run_start` in dmtelnet_recv() tracks where the current run began.
 */
typedef enum
{
    dmtelnet_state_data,      /**< Ordinary data, watching for IAC */
    dmtelnet_state_iac,       /**< Saw IAC, waiting to see what follows */
    dmtelnet_state_cmd,       /**< Saw IAC WILL/WONT/DO/DONT, waiting for the option byte */
    dmtelnet_state_sb_option, /**< Saw IAC SB, waiting for the option byte */
    dmtelnet_state_sb_data,   /**< Collecting subnegotiation payload bytes */
    dmtelnet_state_sb_iac,    /**< Inside a subnegotiation, saw IAC (escape or SE?) */
} dmtelnet_state_t;

struct dmtelnet
{
    dmtelnet_callbacks_t callbacks;
    void* user_data;

    dmtelnet_state_t state;
    uint8_t pending_cmd;                                 /**< Valid while state == dmtelnet_state_cmd */
    uint8_t sb_option;                                   /**< Valid from dmtelnet_state_sb_data onward */
    uint8_t sb_buffer[DMTELNET_MAX_SUBNEGOTIATION_LEN];
    size_t sb_len;
};

static bool is_negotiation_cmd(uint8_t byte)
{
    return byte == DMTELNET_WILL || byte == DMTELNET_WONT || byte == DMTELNET_DO || byte == DMTELNET_DONT;
}

static void emit_data(dmtelnet_t session, const uint8_t* data, size_t data_len)
{
    if (data_len > 0 && session->callbacks.on_data != NULL)
    {
        session->callbacks.on_data(session, data, data_len, session->user_data);
    }
}

/**
 * Shared encode/escape path behind dmtelnet_send() and
 * dmtelnet_send_subnegotiation()'s payload: doubles every literal 0xFF and
 * flushes to on_send in bounded chunks, so an arbitrarily large `data`
 * never requires an equally large scratch buffer.
 */
static void encode_and_send(dmtelnet_t session, const uint8_t* data, size_t data_len)
{
    uint8_t chunk[64];
    size_t chunk_len = 0;

    for (size_t i = 0; i < data_len; i++)
    {
        /* Leave room for a possible 2-byte IAC IAC escape on this byte. */
        if (chunk_len >= sizeof(chunk) - 1)
        {
            session->callbacks.on_send(session, chunk, chunk_len, session->user_data);
            chunk_len = 0;
        }

        chunk[chunk_len++] = data[i];
        if (data[i] == (uint8_t)DMTELNET_IAC)
        {
            chunk[chunk_len++] = (uint8_t)DMTELNET_IAC;
        }
    }

    if (chunk_len > 0)
    {
        session->callbacks.on_send(session, chunk, chunk_len, session->user_data);
    }
}

dmod_dmtelnet_api_declaration(1.0, dmtelnet_t, _create, ( const dmtelnet_callbacks_t* callbacks, void* user_data ))
{
    if (callbacks == NULL || callbacks->on_send == NULL)
    {
        return NULL;
    }

    struct dmtelnet* session = Dmod_Malloc(sizeof(*session));
    if (session == NULL)
    {
        return NULL;
    }

    session->callbacks = *callbacks;
    session->user_data = user_data;
    session->state = dmtelnet_state_data;
    session->sb_len = 0;

    return session;
}

dmod_dmtelnet_api_declaration(1.0, void, _destroy, ( dmtelnet_t session ))
{
    Dmod_Free(session);
}

dmod_dmtelnet_api_declaration(1.0, int, _recv, ( dmtelnet_t session, const uint8_t* data, size_t data_len ))
{
    if (session == NULL || (data == NULL && data_len > 0))
    {
        return -EINVAL;
    }

    size_t run_start = 0;

    for (size_t i = 0; i < data_len; i++)
    {
        uint8_t byte = data[i];

        switch (session->state)
        {
        case dmtelnet_state_data:
            if (byte == (uint8_t)DMTELNET_IAC)
            {
                emit_data(session, data + run_start, i - run_start);
                session->state = dmtelnet_state_iac;
            }
            /* else: still part of the current run - handled once we leave
             * this state or reach the end of the buffer. */
            break;

        case dmtelnet_state_iac:
            run_start = i + 1;
            if (byte == (uint8_t)DMTELNET_IAC)
            {
                /* Escaped 0xFF - a single literal data byte. */
                uint8_t literal = (uint8_t)DMTELNET_IAC;
                emit_data(session, &literal, 1);
                session->state = dmtelnet_state_data;
            }
            else if (is_negotiation_cmd(byte))
            {
                session->pending_cmd = byte;
                session->state = dmtelnet_state_cmd;
            }
            else if (byte == (uint8_t)DMTELNET_SB)
            {
                session->sb_len = 0;
                session->state = dmtelnet_state_sb_option;
            }
            else
            {
                /* NOP, GA, or any other single-byte command this API has no
                 * event for - just resume normal data processing. */
                session->state = dmtelnet_state_data;
            }
            break;

        case dmtelnet_state_cmd:
            run_start = i + 1;
            if (session->callbacks.on_negotiate != NULL)
            {
                session->callbacks.on_negotiate(session, (dmtelnet_cmd_t)session->pending_cmd, byte, session->user_data);
            }
            session->state = dmtelnet_state_data;
            break;

        case dmtelnet_state_sb_option:
            run_start = i + 1;
            session->sb_option = byte;
            session->state = dmtelnet_state_sb_data;
            break;

        case dmtelnet_state_sb_data:
            run_start = i + 1;
            if (byte == (uint8_t)DMTELNET_IAC)
            {
                session->state = dmtelnet_state_sb_iac;
            }
            else if (session->sb_len < DMTELNET_MAX_SUBNEGOTIATION_LEN)
            {
                session->sb_buffer[session->sb_len++] = byte;
            }
            /* else: payload longer than DMTELNET_MAX_SUBNEGOTIATION_LEN -
             * silently truncated, see the header's doc comment. */
            break;

        case dmtelnet_state_sb_iac:
            run_start = i + 1;
            if (byte == (uint8_t)DMTELNET_SE)
            {
                if (session->callbacks.on_subnegotiation != NULL)
                {
                    session->callbacks.on_subnegotiation(session, session->sb_option, session->sb_buffer, session->sb_len, session->user_data);
                }
                session->state = dmtelnet_state_data;
            }
            else if (byte == (uint8_t)DMTELNET_IAC)
            {
                /* Escaped 0xFF inside the subnegotiation payload. */
                if (session->sb_len < DMTELNET_MAX_SUBNEGOTIATION_LEN)
                {
                    session->sb_buffer[session->sb_len++] = (uint8_t)DMTELNET_IAC;
                }
                session->state = dmtelnet_state_sb_data;
            }
            else
            {
                /* Malformed: a new command appeared instead of SE. Drop the
                 * unterminated subnegotiation and reprocess this byte as a
                 * fresh command, exactly like the top-level IAC state does. */
                if (is_negotiation_cmd(byte))
                {
                    session->pending_cmd = byte;
                    session->state = dmtelnet_state_cmd;
                }
                else if (byte == (uint8_t)DMTELNET_SB)
                {
                    session->sb_len = 0;
                    session->state = dmtelnet_state_sb_option;
                }
                else
                {
                    session->state = dmtelnet_state_data;
                }
            }
            break;
        }
    }

    if (session->state == dmtelnet_state_data)
    {
        emit_data(session, data + run_start, data_len - run_start);
    }

    return 0;
}

dmod_dmtelnet_api_declaration(1.0, int, _send, ( dmtelnet_t session, const uint8_t* data, size_t data_len ))
{
    if (session == NULL || (data == NULL && data_len > 0))
    {
        return -EINVAL;
    }

    encode_and_send(session, data, data_len);
    return 0;
}

dmod_dmtelnet_api_declaration(1.0, int, _negotiate, ( dmtelnet_t session, dmtelnet_cmd_t cmd, uint8_t option ))
{
    if (session == NULL || !is_negotiation_cmd((uint8_t)cmd))
    {
        return -EINVAL;
    }

    uint8_t buffer[3] = { (uint8_t)DMTELNET_IAC, (uint8_t)cmd, option };
    session->callbacks.on_send(session, buffer, sizeof(buffer), session->user_data);
    return 0;
}

dmod_dmtelnet_api_declaration(1.0, int, _send_subnegotiation, ( dmtelnet_t session, uint8_t option, const uint8_t* data, size_t data_len ))
{
    if (session == NULL || (data == NULL && data_len > 0))
    {
        return -EINVAL;
    }

    uint8_t header[3] = { (uint8_t)DMTELNET_IAC, (uint8_t)DMTELNET_SB, option };
    session->callbacks.on_send(session, header, sizeof(header), session->user_data);

    if (data_len > 0)
    {
        encode_and_send(session, data, data_len);
    }

    uint8_t trailer[2] = { (uint8_t)DMTELNET_IAC, (uint8_t)DMTELNET_SE };
    session->callbacks.on_send(session, trailer, sizeof(trailer), session->user_data);

    return 0;
}

int dmod_init(const Dmod_Config_t *Config)
{
    return 0;
}

int dmod_deinit(void)
{
    return 0;
}
