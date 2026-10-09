#!/bin/bash
export DISPLAY=:0.0
export QT_QPA_PLATFORM=xcb

# Jetsku u59 perf/memory preset + PortaS5 lighting fix (verified 2026-10-10):
#  - KYTY_SRT_VARIANT_READS=1: the game's GI lighting kernels read SRT pointers
#    no pre-pass can prove; without this their dispatches are skipped -> black 3D.
#  - KYTY_RT_NODE_BUDGET=262144: software-BVH ray budget. The upstream default
#    (8192) starves traversals -> black lighting; a huge budget makes kernels so
#    long they trip the amdgpu ring timeout (GPU hang). 262144 lights the scene
#    and stays under the watchdog. See docs/EXPERIMENTAL.md.
#  - Preset cuts RAM/VRAM use (~9.0 GB -> ~6.8 GB RSS) and speeds up draw prep.
if [ -f "$KYTY_DIR/ally-preset.env" ]; then
    set -a; . "$KYTY_DIR/ally-preset.env"; set +a
fi
export KYTY_SRT_VARIANT_READS=1
export KYTY_RT_NODE_BUDGET=262144

KYTY_DIR="/userdata/system/add-ons/kytyps5"
cd "$KYTY_DIR" || exit 1

TARGET="$1"

# Check if TARGET is a directory and locate eboot.bin inside
if [ -d "$TARGET" ]; then
    if [ -f "$TARGET/eboot.bin" ]; then
        GAME_PATH="$TARGET/eboot.bin"
    elif [ -f "$TARGET/sce_module/eboot.bin" ]; then
        GAME_PATH="$TARGET/sce_module/eboot.bin"
    else
        # Pass the folder path directly if eboot.bin is not in expected spots
        GAME_PATH="$TARGET"
    fi
else
    GAME_PATH="$TARGET"
fi

# Auto-apply a matching game patch (Riivolution-style, non-destructive).
# Looks up the game's TITLE_ID and passes _Patches/<TITLE_ID>.json when present.
PATCH_ARGS=()
if [ -d "$TARGET" ]; then
    TITLE_ID=$(grep -m1 -o '<param key="TITLE_ID">[^<]*' "$TARGET/param.sfx" 2>/dev/null | sed 's/.*>//')
    if [ -n "$TITLE_ID" ] && [ -f "$KYTY_DIR/_Patches/$TITLE_ID.json" ]; then
        PATCH_ARGS=(--game-patch "$KYTY_DIR/_Patches/$TITLE_ID.json")
    fi
fi

if [ -n "$GAME_PATH" ]; then
    "$KYTY_DIR/kyty_emulator" --game "$GAME_PATH" --fullscreen --amd-cpu "${PATCH_ARGS[@]}"
else
    "$KYTY_DIR/kyty_emulator"
fi
