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
| `source/bl_kv.{h,c}` | The flat files' reader: `key=value`, one per line, `#` comments; their `schema` and the round trip of the keys a version does not know. |
| `source/bl_link_conf.{h,c}` | `sd:/config/boreale/link.conf`, the broker's settings every homebrew of the console shares: host, port, TLS, MQTT version, credentials, the console's id and name, the topic and discovery prefixes. |
| `source/bl_app.{h,c}` | `sd:/config/boreale/apps/<module>.conf`, the registry of the Boréale homebrews installed: each writes its own file when it starts (name, version, `.nro`, icon, releases). |

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

## The `key=value` files

A file that several versions of a homebrew read and write (its settings,
`config/boreale/`'s shared files) says its format in a `schema=N` line, a
whole number that goes up when a key changes meaning (a key added keeps
it). `bl_kv` gives each homebrew the same rule:

- `bl_kv_schema()`, or `BlKvFile.schema` after `bl_kv_read()`: the file's
  schema, `1` without the line (the files written before it), `0` when it
  is not a number.
- A file of a newer schema is read for the keys the homebrew knows, and
  never written over: `bl_kv_may_rewrite(&f, MY_SCHEMA)` is false. The
  homebrew works with what it read, in memory.
- Rewriting keeps the keys the homebrew does not know: its callback hands
  them to `bl_kv_keep()`, which copies them into a buffer the caller gives,
  and `bl_kv_write_kept()` appends them, as read, after the caller's own
  keys.
- A line the reader cannot take (a key of 48 bytes or more, a value of 512
  or more) is skipped, and counted: `bl_kv_each()` returns the count,
  `BlKvFile.skipped` keeps it. A file with a line skipped, or with more
  unknown keys than the buffer holds (`BlKvFile.overflow`), is not written
  over either: writing it would lose them.

`bl_kv.h` has the whole pattern, in a few lines.

## The shared broker settings

`sd:/config/boreale/link.conf` (`bl_link_conf`) holds what every homebrew
needs to reach the broker and to say which console it is: the broker's
host and port, TLS and its certificate authority (`config/boreale/ca.pem`
by convention), the MQTT version, the credentials (or `allow_anonymous`),
the console's id (8 lower-case hex digits, made once) and name, and the
topic and discovery prefixes (`boreale`, `homeassistant`). It is set once,
from the screen of the homebrew that offers one (PlayGuard's *Remote
access*), and read by the others and their sysmodules; each homebrew keeps
its own keys (what it publishes, how it takes orders) in its own file.
Being a `bl_kv` file with a `schema`, a homebrew of another version keeps
the keys it does not know when it writes it, and never writes over a file
of a newer schema.

## The registry of the homebrews installed

Each Boréale homebrew writes `sd:/config/boreale/apps/<module>.conf`
(`bl_app`) when it starts, `<module>` being its topics' level
(`playguard`, `playshelf`): its name, its version, where its `.nro` is and
its icon (paths under the SD card's root; no icon: the `.nro`'s own), and
the GitHub repository of its releases. It writes only when something
changed, so the SD card is not written at each start. The others list the
directory (`bl_app_module_of()` on each name) to know who is there: the
shared bell's sections, a screen that lists and launches them, the update
check made for all of them. A `bl_kv` file with a `schema`, like
`link.conf`.

## Tests

```sh
make test          # the host tests, with the address and undefined-behaviour sanitizers
```

`tests/codec` covers the JSON writer, the MQTT codec, the topic filters and
the `key=value` reader with its schema and round trip; `tests/link_conf` the
shared broker settings; `tests/app` the registry's files; `tests/session` runs the session against a fake broker
kept in memory, with a scripted clock. CI runs them under gcc and clang, and
builds every source for the Switch in devkitPro's image.

## License

GPLv3 or later (see [LICENSE](LICENSE)).
