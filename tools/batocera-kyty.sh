#!/bin/bash
export DISPLAY=:0.0
export QT_QPA_PLATFORM=xcb

# RADV handheld (ROG Ally / Radeon 780M) fixes, inherited by the emulator the
# launcher spawns. Without these, Astro's Playroom renders lit geometry black
# (GI kernels' SRT reads skipped + software-BVH node budget starved) — and a
# too-large node budget trips the amdgpu ring timeout (GPU hang).
# See docs/EXPERIMENTAL.md and tools/ally-preset.env in PortaS5 (b10df297).
KYTY_DIR="/userdata/system/add-ons/kytyps5"
if [ -f "$KYTY_DIR/ally-preset.env" ]; then
    set -a; . "$KYTY_DIR/ally-preset.env"; set +a
fi
export KYTY_SRT_VARIANT_READS=1
export KYTY_RT_NODE_BUDGET=262144

cd "$KYTY_DIR"
./launcher