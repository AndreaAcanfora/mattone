#!/bin/bash
# Runs the test ROMs through build/mattone-run and reports each result.
set -u
cd "$(dirname "$0")/.."
RUN=build/mattone-run
B=tests/roms/blargg
M=tests/roms/mooneye
failed=0

pass() { printf '  pass  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; failed=$((failed + 1)); }

# Blargg's ROMs print "Passed" or "Failed" when they finish. Extra arguments
# (--dmg, --cgb) go to the runner.
blargg() { # rom [args]
    out=$($RUN --frames 6000 --serial --until Passed "${@:2}" "$B/$1" 2>&1)
    if [ $? -eq 0 ]; then pass "$1"; else fail "$1"; printf '%s\n' "$out" | sed 's/^/        /'; fi
}

# mooneye's ROMs end with LD B,B and the Fibonacci numbers in B-L on success.
mooneye() { # rom [args]
    if $RUN --frames 1200 --mooneye "${@:2}" "$M/$1" >/dev/null; then pass "$1 ${*:2}"; else fail "$1 ${*:2}"; fi
}

# mealybug: the screen when the ROM reaches LD B,B must match the reference.
mealybug() { # name
    shot=$(mktemp)
    $RUN --frames 600 --mooneye --screenshot "$shot" "tests/roms/mealybug/$1.gb" >/dev/null
    if result=$(python3 tests/compare.py "$shot" "tests/roms/mealybug/$1.png"); then pass "mealybug $1"; else fail "mealybug $1"; echo "        $result"; fi
    rm -f "$shot"
}

# Screen tests: the ROM halts on a final image that must match the reference.
screen() { # name rom reference frames
    shot=$(mktemp --suffix=.img)
    $RUN --frames "$4" --screenshot "$shot" "$2" && result=$(python3 tests/compare.py "$shot" "$3")
    if [ $? -eq 0 ]; then pass "$1"; else fail "$1"; echo "        $result"; fi
    rm -f "$shot"
}

# These ROMs run on both machines: check both.
for model in --dmg --cgb; do
    echo "CPU instructions ($model)"
    for rom in "$B"/cpu_instrs/individual/*.gb; do blargg "cpu_instrs/individual/$(basename "$rom")" $model; done
    blargg cpu_instrs/cpu_instrs.gb $model
    echo "Timing ($model)"
    blargg instr_timing/instr_timing.gb $model
    for rom in "$B"/mem_timing/individual/*.gb; do blargg "mem_timing/individual/$(basename "$rom")" $model; done
    blargg mem_timing/mem_timing.gb $model
    blargg mem_timing-2/mem_timing.gb $model
done
echo "HALT"
screen halt_bug "$B/halt_bug.gb" tests/expected/halt_bug.png 300
echo "OAM corruption bug"
# The single 7-timing_effect ROM writes a detailed report that outgrows its
# 8 KiB of cartridge RAM and runs into its own code in WRAM; the combined ROM
# runs the same test and checks it by CRC.
for rom in "$B"/oam_bug/rom_singles/*.gb; do
    case "$rom" in *7-timing_effect.gb) continue ;; esac
    blargg "oam_bug/rom_singles/$(basename "$rom")" --dmg
done
blargg oam_bug/oam_bug.gb --dmg
echo "Sound"
for rom in "$B"/dmg_sound/rom_singles/*.gb; do blargg "dmg_sound/rom_singles/$(basename "$rom")"; done
blargg dmg_sound/dmg_sound.gb
python3 tests/tone-test.py "$RUN" || failed=$((failed + 1))
echo "Game Boy Color sound"
for rom in "$B"/cgb_sound/rom_singles/*.gb; do blargg "cgb_sound/rom_singles/$(basename "$rom")" --cgb; done
blargg cgb_sound/cgb_sound.gb --cgb
echo "PPU"
screen dmg-acid2 tests/roms/dmg-acid2.gb tests/roms/dmg-acid2.png 60
screen "mooneye manual-only/sprite_priority" "$M/manual-only/sprite_priority.gb" tests/roms/sprite_priority.png 120
echo "Colours for original Game Boy games on a CGB"
build/palette-test || failed=$((failed + 1))
echo "Game Boy Color PPU"
screen cgb-acid2 tests/roms/cgb-acid2.gbc tests/roms/cgb-acid2.png 60
echo "Mid-scanline register changes"
for t in m2_win_en_toggle m3_bgp_change m3_bgp_change_sprites m3_obp0_change m3_scx_low_3_bits; do mealybug "$t"; done
echo "mooneye (every test that applies to the DMG)"
for rom in $(cd "$M" && find acceptance emulator-only -name '*.gb' | sort); do
    case "$(basename "$rom" .gb)" in
    # other models: SGB, MGB, CGB, AGB/AGS, and the first DMG revision
    *-S | *-C | *-A | *-sgb | *-sgb2 | *-mgb | *-dmg0 | *-cgb*) continue ;;
    esac
    mooneye "$rom"
done
echo "mooneye on a CGB, in compatibility mode (every test that applies to it)"
for rom in $(cd "$M" && find acceptance emulator-only misc -name '*.gb' | sort); do
    case "$(basename "$rom" .gb)" in
    # DMG, MGB and SGB only, other models, and the first CGB revision
    *-GS | *-S | *-A | *-dmg* | *-mgb | *-sgb* | *-cgb0) continue ;;
    esac
    mooneye "$rom" --cgb
done

if [ "$failed" -ne 0 ]; then
    echo "$failed test(s) failed"
    exit 1
fi
echo "all tests passed"
