// sync_relay — the homebrews that relay their documents through the agent
// (bl:link), as `capabilities` lists them while their session is open. A header
// of its own, with nothing else in it: the engine and the state's builders both
// need the type, and the app's classes share names with the rest of the sync
// headers.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// A relayed module: its name and its homebrew's version only, its documents
// carry their own `schema`.
typedef struct {
    char name[33];       // boreale-link's BL_APP_MODULE_MAX
    char version[24];
} SyncRelayModule;

#ifdef __cplusplus
}
#endif
