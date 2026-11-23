# The shared agent — the remote link's background sysmodule (see source/main.c).
# Produces out/boreale-link.nsp, which an app's release zip places at
# atmosphere/contents/4200000000504741/exefs.nsp and which the apps bundle in
# their romfs. It links PlayGuard's module (../modules/playguard: the C service
# layer and the remote link's documents and orders), this library (../source) and
# the kit's console helpers (boreale-kit's source/core).
#
# Built with boreale-kit beside this library: from inside an app it is the sibling of this library there
# (library/boreale-kit next to library/boreale-link). The library does not
# carry the kit, so a build of its own (ci.yml, a release) fetches it at agent/KIT_REF; dist.sh makes the release's files.
#
# AGENT_VERSION: the version the app shows for it (the app passes its own).

# TOPDIR: this folder, in both passes (the second runs from build/).
TOPDIR      ?= $(CURDIR)
TARGET      := boreale-link
LINK        := ..
PG          := ../modules/playguard
KIT         := ../../boreale-kit/source
SOURCES     := source $(PG)/core $(PG)/sync $(PG)/util $(KIT)/core $(LINK)/source
INCLUDES    := source $(KIT) $(KIT)/core $(PG) $(PG)/core $(PG)/sync $(LINK)/source
APP_JSON    := $(TOPDIR)/boreale-link.json

AGENT_VERSION ?= 0.0.0
EXTRA_CFLAGS := -DAGENT_VERSION=\"$(AGENT_VERSION)\"

CFILES      := main.c agent_core.c agent_ipc.c agent_relay.c \
               pctl_ops.c pctl_dump.c pure.c rescue.c write_guard.c time_ops.c calendar.c local_time.c sysinfo.c \
               playstats.c playstats_logic.c playlog.c nacp.c nacp_read.c \
               console_info.c console_info_pure.c \
               bl_json.c mqtt_packet.c mqtt_client.c bl_kv.c bl_link_conf.c sync_conf.c sync_apply.c sync_records.c sync_exec.c \
               sync_entities.c sync_discovery.c sync_state.c sync_system.c sync_watch.c sync_engine.c \
               bl_net.c bl_tls.c bl_session.c sync_icons.c sync_titles.c sync_notices.c bl_jread.c bl_app.c bl_notices.c bl_relay.c \
               bl_latest.c bl_http.c bl_ntp.c bl_agent_conf.c
# nacp.c reads compressed game titles (zlib).
EXTRA_LIBS  := -lz

include $(TOPDIR)/common.mk
