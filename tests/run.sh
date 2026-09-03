#!/bin/sh

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
include="$root/boards/shields/prospector_adapter/include"
reducer="$root/boards/shields/prospector_adapter/src/touch_brightness_drag.c"
gate="$root/boards/shields/prospector_adapter/src/touch_brightness_gate.c"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

travel=$(awk '/^config PROSPECTOR_TOUCH_BRIGHTNESS_TRAVEL$/ {found = 1; next}
              found && /^(config|menuconfig|menu|endmenu|choice|endchoice|if|endif|source|rsource)/ {exit}
              found && $1 == "default" {print $2; exit}' "$root/Kconfig")
if [ -z "$travel" ]; then
    echo "no default found for PROSPECTOR_TOUCH_BRIGHTNESS_TRAVEL in $root/Kconfig" >&2
    exit 2
fi

cc -std=c99 -Wall -Wextra -Werror -I"$include" \
   -DCONFIG_PROSPECTOR_TOUCH_BRIGHTNESS_TRAVEL="$travel" \
   -o "$out/touch_brightness_drag_test" \
   "$root/tests/touch_brightness_drag_test.c" "$reducer"

"$out/touch_brightness_drag_test"

cc -std=c11 -Wall -Wextra -Werror -I"$include" \
   -o "$out/touch_brightness_gate_test" \
   "$root/tests/touch_brightness_gate_test.c" "$gate"

"$out/touch_brightness_gate_test"
