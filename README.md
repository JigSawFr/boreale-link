# boreale-link

The remote link the Boréale homebrews share: an MQTT 5.0 / 3.1.1 client, the
session with the broker, TLS through the Switch's own `ssl` service, a JSON
writer and the reader of the small `key=value` files. Written in C, with no
allocation and no thread of its own, so the same code runs in an app and in
a sysmodule with a static heap.

It knows nothing of any homebrew in particular: what goes over the link (the
documents, the orders, the Home Assistant discovery) belongs to the homebrew
that uses it. [PlayGuard](https://github.com/JigSawFr/PlayGuard) is the first
of them; this code comes from its `source/link/`.

## What is in it

| File | What it does |
|---|---|
| `source/mqtt_packet.{h,c}` | MQTT 5.0 and 3.1.1 packets, encoded into and decoded from caller buffers, and topic filters. No I/O. |
| `source/mqtt_client.{h,c}` | A small client over any byte stream (`BlIo`): CONNECT with a last will, PUBLISH (QoS 0, whole or in pieces), SUBSCRIBE, keep-alive. |
| `source/bl_session.{h,c}` | One session with the broker: the last will `offline` and `online` on `<base>/availability`, the fallback from 5.0 to 3.1.1, the pause between attempts (5 s doubling up to 5 min), a console that slept past the keep-alive, publishing under `<base>`. |
| `source/bl_net.{h,c}` | The plain TCP stream (POSIX sockets: libnx on the Switch, the host's elsewhere) and a monotonic clock. |
| `source/bl_tls.{h,c}` | The stream over TLS through the console's `ssl` service, with an optional private certificate authority. Switch only. |
| `source/bl_json.{h,c}` | A JSON writer into a fixed buffer. |
| `source/bl_kv.{h,c}` | The flat files' reader: `key=value`, one per line, `#` comments. |

## Using it

Add it as a submodule (`library/boreale-link` by convention), compile
`source/*.c` with the homebrew's own toolchain and flags, and put `source/`
on the include path. A caller owns one `BlSession`, gives it where and who
(`BlSessionConf`), and drives it from its loop:

```c
bool connected;
const int sleep = bl_session_connect_due(&s, max_wait_ms, &connected);
if (sleep >= 0) return sleep;          // not connected yet: wait that long
if (connected) { /* publish what a new connection needs */ }
/* the homebrew's own work: bl_session_publish(&s, "state", …) */
if (bl_session_check(&s)) bl_session_poll(&s, wait_ms);
```

The session calls back to subscribe once connected, and hands every message
the subscriptions bring to the caller's `on_message`.

## Tests

```sh
make test          # the host tests, with the address and undefined-behaviour sanitizers
```

`tests/codec` covers the JSON writer, the MQTT codec, the topic filters and
the `key=value` reader; `tests/session` runs the session against a fake broker
kept in memory, with a scripted clock. CI runs them under gcc and clang, and
builds every source for the Switch in devkitPro's image.

## License

GPLv3 or later (see [LICENSE](LICENSE)).
