# telnetd

A Telnet server built on [dmtelnet](../../README.md). Rather than owning a
shell itself, telnetd is a [dmdrvi](https://github.com/choco-technologies/dmdrvi)
driver: each accepted TCP connection becomes a dynamically-announced device,
which [dmdevfs](https://github.com/choco-technologies/dmdevfs) exposes under
`/dev`, which [dmtty](https://github.com/choco-technologies/dmtty) turns into
a terminal node, which its own `console@.ini`/`console.rules` (see
`dmtty/tools/console`) starts a `dmell` login shell on - exactly the same
chain a physical UART goes through. telnetd itself never links against
dmtty or dmell.

```
dmtcp (TCP) <-> dmtelnet (protocol) <-> telnetd (dmdrvi) <-> dmdevfs/dmvfs <-> dmtty <-> dmell
```

## Configuration

telnetd is a dmdevfs driver, configured the same way any other one is (see
dmdevfs's own `docs/README.md`): drop `configs/telnetd.ini` into dmdevfs's
scanned config directory.

```ini
[main]
driver_name=telnetd
port=23
```

On dmod-boot, add a `driver=` entry to `flash.dmd`:

```
telnetd driver=telnetd.ini
```

## Known limitations

- `dmtcp_send()` is best-effort: telnetd does not yet implement
  backpressure (`dmtcp_writable_handler_t`) all the way through to dmtty, so
  a short write under sustained load is silently dropped. Not observed to
  matter for interactive terminal traffic.
- A `dmell` session ending does not itself hang up the TCP connection -
  `console@.ini` leaves the tty node attached, waiting for a new session,
  matching how a physical console behaves. Only the peer disconnecting (or
  the tty node being explicitly detached) closes the connection.

## Author

Patryk Kubiak

## License

MIT (see the repository's [LICENSE](../../LICENSE))
