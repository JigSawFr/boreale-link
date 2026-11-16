# boreale-link — the remote link in the background

An optional Atmosphère sysmodule that keeps PlayGuard's remote link (MQTT,
Home Assistant: [`docs/home-assistant.md`](https://github.com/JigSawFr/PlayGuard/blob/main/docs/home-assistant.md)) up
while PlayGuard is closed. Without it the link only runs while PlayGuard is
open; with it, the console stays online, publishes its state and play time,
and carries out the orders Home Assistant sends, whatever is running.

It is **optional**: PlayGuard carries a copy and installs it from *Console ›
Optional modules*, which also starts it, stops it, updates it and removes it;
it is also its own download (`boreale-link.zip`, to extract at the SD-card
root). It does nothing until *Console › Remote access* is set up.

When the agent on the SD card is not the one PlayGuard carries (another
build, or another protocol of `pg:agent`), PlayGuard offers the update at
start-up (*Later* skips that build). An update asks the agent to leave the
broker (`bl:link`'s Shutdown), stops it, keeps the previous `exefs.nsp` as
`exefs.nsp.bak`, starts the new one and waits up to 10 s for its `Hello`; when
it does not answer, the new one is stopped and the previous one put back and
started again (PlayGuard's `source/action/agent_update.cpp` on the kit's `sysmodule_flow`, host-tested in
boreale-kit's `tests/sysmodule_flow/`).

## What it does

- Connects to the broker set in `sd:/config/playguard/sync.conf` (the same
  settings as PlayGuard, same session engine: `modules/playguard/sync/sync_engine.c` on
  boreale-link's `bl_session.c`),
  as the client `bl-<id>-link`, with `offline` as its last will. `<id>` is
  this system's: on an emuMMC it differs from the sysMMC's, so the two are
  two devices ([`docs/sync-protocol.md`](https://github.com/JigSawFr/PlayGuard/blob/main/docs/sync-protocol.md)).
- Every `poll_s` seconds, reads the parental controls and the play timer
  through PlayGuard's own service layer (`modules/playguard/core/`), and the play log of
  the last 7 days (`pdm:qry`, no game names: those come from `names.txt`,
  which PlayGuard writes), and publishes `state`, `activity` and, after
  midnight, the finished day.
- Publishes the icon of the game played when the broker does not have it
  yet and the Boréale homebrews' shared icon cache on the SD card does
  (`config/boreale/cache/icons/`, boreale-link's `bl_cache`), streamed from
  the file a piece at a time;
  every other icon is PlayGuard's to publish (no `ns` here to read one).
- Carries out orders **only under the *auto* policy**, through the same gate,
  unlock and relock as PlayGuard (`modules/playguard/sync/sync_exec.c`). Under *ask*,
  orders wait on the broker until PlayGuard opens and asks on the console.
- Puts the usual limit back by itself the day after extra time, as PlayGuard
  does (not while the lower limit would stop the game in progress, on the
  same weekday a week later). The day after "no more play today", or when
  the usual limit was "no limit", it leaves the question to PlayGuard.
- Changes nothing when PlayGuard was in read-only mode, or when the firmware
  is not the one PlayGuard last ran on (a system update PlayGuard has not
  checked): `nro_state.txt`.

## With PlayGuard open

The agent stays the only MQTT client, so Home Assistant never sees the
console go offline. PlayGuard holds a session on the agent's IPC service
`pg:agent` ([`agent_ipc.h`](../modules/playguard/sync/agent_ipc.h)): in the
foreground it is the only one reading the parental controls and pushes what it
reads; orders go to PlayGuard, which applies its policy, its PIN check and its
change history. When PlayGuard closes or goes to the background, the agent
reads the console again by itself. *Console › Remote access* then says
*Online · through the agent*; when PlayGuard opens, it adopts the agent's
records and imports what the agent changed into its history. Started or
stopped from *Console › Optional modules*, the link moves between the agent and
PlayGuard's own session at once.

## Without PlayGuard's module

PlayGuard's part of the agent (reading the parental controls, the state and
activity documents, the discovery, carrying out its orders) runs only while
PlayGuard's registry file `sd:/config/boreale/apps/playguard.conf` has
`publish=1` and the `.nro` it names is on the SD card (PlayGuard writes the
file at each start). Otherwise the agent keeps to the console's own documents
(`availability`, `system`, `notices`, `capabilities`) and the homebrews that
relay through it, and `playguard` is not in `capabilities`. The file is read
every few seconds: the module comes on or goes off without restarting the
agent, and the orders the broker kept come back when it is on again. An agent
installed by hand before PlayGuard has run once stays in this state.

## Files

In `sd:/config/playguard/`, as PlayGuard's: it reads `sync.conf` and
`sync/profiles.txt`, shared by every system on the SD card; in this system's
folder, `nand/<system>/sync/` (`sysmmc`, `emummc` or `unknown`, as PlayGuard
names it), it reads `nro_state.txt`, `names.txt` and `icons.txt` and writes
`agent_icons.txt` (the icons it published itself),
`agent_state.txt` (what it changed: extra time, console lock, a relock
pending), `agent_events.log` (the orders it carried out, imported into
PlayGuard's history) and `agent_watchdog.log` (where its loop stopped, when
it stopped; PlayGuard's report shows the last lines); a report asked for from
Home Assistant goes to
`logs/reports/` with PlayGuard's. At boot it waits up to 10 s for the SD card,
then tries again every 30 s.
Details: [`docs/sync-protocol.md`](https://github.com/JigSawFr/PlayGuard/blob/main/docs/sync-protocol.md).

## Build

The agent is built from inside an app, which has this library and boreale-kit
side by side (`library/boreale-link`, `library/boreale-kit`): the kit's
`source/core` is part of it, and the library does not carry the kit.

```sh
make -C library/boreale-link/agent AGENT_VERSION=1.2.0   # -> out/boreale-link.nsp (needs devkitPro, DEVKITPRO set)
```

PlayGuard's `make agent`, `make dist-agent` and `make` (the .nro with this module
bundled in its romfs) do that; so will PlayShelf's.

It links PlayGuard's module (`../modules/playguard/`: its C service layer in
`core/`, the remote link's documents and orders in `sync/`, the play log in
`util/`), this library (`../source/`) and the kit's console helpers; its own
code is `source/main.c` (the loop), `source/agent_core.c` (what the loop and the
IPC thread share), `source/agent_ipc.c` (the HIPC server) and
`source/agent_relay.c` (the `bl:link` sessions). Their host tests are the
apps': PlayGuard's `tests/` build them from this submodule.

Program ID `0x4200000000504741`, in the private range homebrew sysmodules use.
Static heap 1.5 MiB, 256 KiB of it the network service's room for the socket
buffers; one TCP socket, on two `bsd` sessions (the watchdog shuts it down
when a call on it hangs); TLS through the console's `ssl` service.

PlayGuard's `make test` also runs `tests/agent_e2e/`: the session engine and the relay
service, assembled as `main.c` does without libnx, against a real MQTT broker
(mosquitto, its command-line clients watching and driving it). It covers the
console's documents on connection, a relayed homebrew, PlayGuard's orders, and
the module going off and on. Without mosquitto it skips itself; CI installs it
and sets `REQUIRE_MOSQUITTO`, which makes that a failure.

## Not tested on a console yet

Everything here builds and its logic is host-tested, but the IPC server, the
start without a reboot from PlayGuard (`pm:shell`), TLS and the share of the
`pctl:a` service with the HOME menu's PIN prompt still have to be tried on
hardware.

## License

GPLv3 (see [`../../LICENSE`](../LICENSE)).
