#!/bin/sh
# Runs the test ROMs through build/mattone-run and reports each result.
set -u
cd "$(dirname "$0")/.."
RUN=build/mattone-run
B=tests/roms/blargg
failed=0

pass() { printf '  pass  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; failed=$((failed + 1)); }

# Blargg's ROMs print "Passed" or "Failed" when they finish.
blargg() { # rom
    out=$($RUN --frames 6000 --serial --until Passed "$B/$1" 2>&1)
    if [ $? -eq 0 ]; then pass "$1"; else fail "$1"; printf '%s\n' "$out" | sed 's/^/        /'; fi
}

# Screen tests: the ROM halts on a final image that must match the reference.
screen() { # name rom reference frames
    shot=$(mktemp)
    $RUN --frames "$4" --screenshot "$shot" "$2" && result=$(python3 tests/compare.py "$shot" "$3")
    if [ $? -eq 0 ]; then pass "$1"; else fail "$1"; echo "        $result"; fi
    rm -f "$shot"
}

echo "CPU instructions"
for rom in "$B"/cpu_instrs/individual/*.gb; do blargg "cpu_instrs/individual/$(basename "$rom")"; done
blargg cpu_instrs/cpu_instrs.gb
echo "Timing"
blargg instr_timing/instr_timing.gb
for rom in "$B"/mem_timing/individual/*.gb; do blargg "mem_timing/individual/$(basename "$rom")"; done
blargg mem_timing/mem_timing.gb
blargg mem_timing-2/mem_timing.gb
echo "HALT"
screen halt_bug "$B/halt_bug.gb" tests/expected/halt_bug.png 300
echo "PPU"
screen dmg-acid2 tests/roms/dmg-acid2.gb tests/roms/dmg-acid2.png 60

if [ "$failed" -ne 0 ]; then
    echo "$failed test(s) failed"
    exit 1
fi
echo "all tests passed"
