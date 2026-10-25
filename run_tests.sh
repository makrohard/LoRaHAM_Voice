#!/usr/bin/env bash
# Voice's own tests: the socket options with the source compiled in
# (LORAHAM_VOICE_NO_MAIN, -DNO_GTK), then the built CLI binary as a process.
# `./run_tests.sh gui`: the GTK band combo test instead; it needs a display
# (CI: xvfb-run -a ./run_tests.sh gui) and fails without one.
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
libs=(-lcodec2 -lasound -lncurses -lpthread -lm)
if [ "${1:-}" = gui ]; then
    # shellcheck disable=SC2046  # pkg-config prints several words on purpose
    cc -std=gnu11 -Wall -o "$out/test_gui_band" tests/test_gui_band.c $(pkg-config --cflags --libs gtk+-3.0) "${libs[@]}"
    "$out/test_gui_band"
    exit 0
fi
cc -std=gnu11 -Wall -DNO_GTK -o "$out/test_socket_options" tests/test_socket_options.c "${libs[@]}"
"$out/test_socket_options"
cc -std=gnu11 -Wall -DNO_GTK -o "$out/loraham_voice_cli" loraham_voice_v107.c "${libs[@]}"
# shellcheck disable=SC2046  # pkg-config prints several words on purpose
cc -std=gnu11 -Wall -o "$out/loraham_voice" loraham_voice_v107.c $(pkg-config --cflags --libs gtk+-3.0) "${libs[@]}"
cc -std=gnu11 -Wall -o "$out/test_voice_binary" tests/test_voice_binary.c -lutil
"$out/test_voice_binary" "$out/loraham_voice_cli" "$out/loraham_voice"
