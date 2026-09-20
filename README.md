# dmtelnet

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmtelnet/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/choco-technologies/dmtelnet/actions/workflows/ci.yml)

dmtelnet DMOD library module.

## Description

A transport-agnostic Telnet protocol engine (RFC 854): IAC command parsing,
WILL/WONT/DO/DONT option negotiation, subnegotiation (`IAC SB ... IAC SE`),
and 0xFF byte-stuffing in both directions. dmtelnet never touches a socket
itself - it is fed raw bytes via `dmtelnet_recv()` and produces raw bytes
via a callback, so it can sit on top of `dmtcp`, `dmudp`, a UART, or a unit
test's own loopback buffer.

See `tools/telnetd` for `telnetd`, a Telnet server built on this library
that serves an interactive `dmell` login shell over TCP.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

Tests are built automatically alongside the module (see `tests/`). Once built,
run them with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs the test module's dependencies with `dmf-get` and then runs
it through `dmod_loader`. To run it manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmtelnet-local.dmd -y
dmod_loader build/dmf/test_dmtelnet.dmf
```

## Usage

This library module provides functions that can be used by other modules:

```c
#include "dmtelnet.h"
```

See [docs/api-reference.md](docs/api-reference.md) for a worked example.

## API

| Function | Description |
|----------|-------------|
| `dmtelnet_create(callbacks, user_data)` | Create a session. `callbacks->on_send` is required. |
| `dmtelnet_destroy(session)` | Destroy a session created by `_create()`. |
| `dmtelnet_recv(session, data, data_len)` | Feed newly-received raw bytes into the session. |
| `dmtelnet_send(session, data, data_len)` | Send application data (IAC-escaped). |
| `dmtelnet_negotiate(session, cmd, option)` | Send `IAC <cmd> <option>`. |
| `dmtelnet_send_subnegotiation(session, option, data, data_len)` | Send `IAC SB <option> <data> IAC SE`. |

See [include/dmtelnet.h](include/dmtelnet.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmtelnet`.
## Project Structure

```
dmtelnet/
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   └── dmtelnet.h
├── src/
│   └── dmtelnet.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmtelnet_test.c
├── tools/
│   └── telnetd/       # Telnet server (dmdrvi driver -> dmell over TCP)
├── CMakeLists.txt
├── Makefile
├── dmtelnet.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
