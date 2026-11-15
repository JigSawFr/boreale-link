// nand_key — the folder of one system's own data on the SD card,
// <data folder>/nand/<key>: "sysmmc", "emummc" or "unknown" (Atmosphère not
// detected, or the emuMMC state not read). Plain C with no libnx, shared by
// the app (util/nand.hpp) and the agent sysmodule, which must find the same
// folder.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>

static inline const char *nand_key_name(bool known, bool emummc)
{
    if (!known) return "unknown";
    return emummc ? "emummc" : "sysmmc";
}
