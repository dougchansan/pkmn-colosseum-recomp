// SPDX-License-Identifier: GPL-3.0-or-later

#include "moderngekko/mod_abi.h"

// Host-rendering feature packages do not replace guest code. The exported
// marker gives tooling a stable capability name while the package descriptor
// supplies the normal ModernGekko identity, version, game check, and netplay
// fingerprint.
static void feature_marker(CPUState *state) {
  moderngekko_mod_return_u32(state, 1u);
}

static const ModernGekkoModExportEntry exports[] = {
    RECOMP_EXPORT(MOD_FEATURE_EXPORT, feature_marker),
};

static const ModernGekkoModDesc descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GC6E01",
    MOD_ID,
    MOD_VERSION,
    MOD_DISPLAY_NAME,
    0,
    0u,
    0,
    0u,
    0,
    0u,
    exports,
    1u,
    0,
    0u,
    0,
    0u,
    0,
    0u,
    0,
    0,
};

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc *moderngekko_get_mod(void) {
  return &descriptor;
}
