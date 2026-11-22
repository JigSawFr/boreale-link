#!/bin/sh
# What the agent's release is made of, from the build in out/ (make, with
# AGENT_VERSION): out/boreale-link.zip (the SD card's layout, reproducible: one
# date for every file, a fixed order), out/boreale-link.json (the manifest the
# homebrews read: version, the bl:link protocol it speaks, the nsp's SHA-256, the
# kit it was built with) and out/SHA256SUMS.txt. `agent/README.md` says how an
# app takes it.
#
#   AGENT_VERSION=1.2.0 agent/dist.sh        (after `make -C agent AGENT_VERSION=1.2.0`)
set -eu

cd "$(dirname "$0")"
: "${AGENT_VERSION:?AGENT_VERSION is the version the release carries (1.2.0)}"
TID=4200000000504741
NSP=out/boreale-link.nsp
[ -f "$NSP" ] || { echo "dist.sh: $NSP is missing: build the agent first (make)" >&2; exit 1; }

STAGE=out/dist
rm -rf "$STAGE" out/boreale-link.zip out/boreale-link.json out/SHA256SUMS.txt
DIR="$STAGE/atmosphere/contents/$TID"
mkdir -p "$DIR/flags"
cp "$NSP" "$DIR/exefs.nsp"
: > "$DIR/flags/boot2.flag"
# Read by the sysmodule managers (Hekate Toolbox, ovl-sysmodules).
printf '{\n  "name": "Boreale Link",\n  "tid": "%s",\n  "requires_reboot": false\n}\n' "$TID" > "$DIR/toolbox.json"
cp ../LICENSE "$DIR/LICENSE.txt"

EPOCH="${SOURCE_DATE_EPOCH:-0}"
(cd "$STAGE" && find . -exec touch -h -d "@$EPOCH" {} + && find . -mindepth 1 | LC_ALL=C sort | TZ=UTC zip -X -q -@ ../boreale-link.zip)
rm -rf "$STAGE"

PROTOCOL=$(sed -n 's/^#define BL_RELAY_PROTOCOL \([0-9][0-9]*\).*/\1/p' ../source/bl_relay.h)
[ -n "$PROTOCOL" ] || { echo "dist.sh: BL_RELAY_PROTOCOL not found in source/bl_relay.h" >&2; exit 1; }
SHA=$(sha256sum "$NSP" | cut -d' ' -f1)
KIT=$(tr -d '[:space:]' < KIT_REF)
printf '{\n  "schema": 1,\n  "name": "boreale-link",\n  "version": "%s",\n  "protocol": %s,\n  "tid": "0x%s",\n  "sha256": "%s",\n  "kit": "%s"\n}\n' \
    "$AGENT_VERSION" "$PROTOCOL" "$TID" "$SHA" "$KIT" > out/boreale-link.json
(cd out && sha256sum boreale-link.zip boreale-link.nsp boreale-link.json > SHA256SUMS.txt)
ls -l out/boreale-link.zip out/boreale-link.nsp out/boreale-link.json out/SHA256SUMS.txt
