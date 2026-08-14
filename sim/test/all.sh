#!/usr/bin/env bash
# Run every headless engine test. Requires sim/engine/build.sh to have been run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
roster_test_bin="$(mktemp "${TMPDIR:-/tmp}/ha-flipper-roster.XXXXXX")"
adapter_test_bin="$(mktemp "${TMPDIR:-/tmp}/ha-adapter-config.XXXXXX")"
trap 'rm -f "$roster_test_bin" "$adapter_test_bin"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror flipper-roster.c -o "$roster_test_bin"
"$roster_test_bin"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I../engine adapter-config.cpp -o "$adapter_test_bin"
"$adapter_test_bin"
for t in smoke.mjs identity.mjs trivia.mjs duel.mjs result-attribution.mjs draw-input.mjs packs.mjs reactions.mjs content.mjs \
         guesscolor.mjs battleship.mjs spectrum.mjs kmk.mjs secrets.mjs fillblank.mjs \
         gamevote.mjs wyr.mjs utf8.mjs lang.mjs chess.mjs werewolf.mjs spyfall.mjs \
         frankendraw.mjs multigame.mjs; do
    node "$t"
done
echo "all engine tests passed"
