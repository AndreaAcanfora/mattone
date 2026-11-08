<div align="center">

# mattone

**A Game Boy and Game Boy Color emulator written in C11, with sound. The same core runs natively and in your browser as a 27 KB WebAssembly module. It passes every DMG test of the mooneye suite, dmg-acid2 and cgb-acid2.**

*Mattone* is Italian for "brick", the nickname the original Game Boy earned.

### [▶ Play it in your browser](https://andreaacanfora.github.io/mattone/)

[![CI](https://github.com/AndreaAcanfora/mattone/actions/workflows/ci.yml/badge.svg)](https://github.com/AndreaAcanfora/mattone/actions/workflows/ci.yml)
[![C11](https://img.shields.io/badge/C-11-A8B9CC?logo=c&logoColor=white)](src)
[![WebAssembly](https://img.shields.io/badge/wasm-27_KB-654FF0?logo=webassembly&logoColor=white)](src/wasm.c)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Donate with PayPal](https://img.shields.io/badge/donate-PayPal-00457C?logo=paypal)](https://www.paypal.com/paypalme/AndreaAca)

![mattone running Tobu Tobu Girl Deluxe in colour: the title screen, the level menu, then the girl bouncing off bats as she climbs](docs/demo.gif)

<sub>Recorded from the emulator itself (`mattone-run --dump`, then [scripts/make-gif.py](scripts/make-gif.py)).
Game: [Tobu Tobu Girl Deluxe](https://github.com/SimonLarsen/tobutobugirl-dx) by Tangram Games.</sub>

</div>

## What it emulates

| Part | Status |
| --- | --- |
| **CPU** (Sharp SM83) | every opcode including the CB prefix, cycle-accurate per memory access, interrupts, `HALT` and the HALT bug, `EI` delay, the OAM corruption bug of 16-bit increments and OAM accesses during the OAM scan |
| **Timer** | DIV/TIMA/TMA/TAC with the falling-edge behaviour, the overflow delay and the reload write races |
| **PPU** | a pixel pipeline like the hardware's: a background/window fetcher feeding a pixel FIFO, a sprite FIFO, pixels out one per dot, so registers changed in the middle of a line take effect at the right pixel (palette writes, fine scroll); background, window (with its own line counter), 8×8 and 8×16 sprites, the 10-per-line limit, DMG priority rules; mode 3 stretched by scrolling, the window and each sprite; STAT and LY=LYC timing, OAM/VRAM lock-out and the LCD switch-on line exact to the M-cycle; the boot ROM's logo left in VRAM |
| **OAM DMA** | start delay, restart, 160 cycles at one byte per cycle, bus conflicts with the CPU |
| **Cartridges** | ROM only, MBC1 (and MBC1M multicarts), MBC2, MBC3 with the real-time clock, MBC5; battery saves |
| **Joypad, serial** | buttons with the joypad interrupt; the link port with nothing plugged in, clocked at 8192 Hz in step with the system clock |
| **Sound** | both square channels (with sweep), wave and noise; length, envelope and sweep units on the 512 Hz frame sequencer; DMG quirks such as wave RAM access while playing; stereo panning and master volume; a high-pass filter like the real output capacitor |
| **Game Boy Color** | CGB games run on a CGB: double speed, the second VRAM bank and eight WRAM banks, colour palette RAM, tile attributes (palette, bank, flips, priority), sprites from either bank with CGB priority by OAM index, general-purpose and H-blank VRAM DMA, the CGB's sound differences, the 2050 M-cycle pause of a speed switch. Original Game Boy games run on a DMG, or on a CGB in its compatibility mode (CGB timing, sound and boot state, DMG graphics; the colours are the DMG palette you pick, not the CGB boot ROM's colourisation) |

## Accuracy

The test ROMs run on every push, on GCC and Clang, and again under AddressSanitizer and UBSan:

| Test ROM | Result |
| --- | --- |
| Blargg `cpu_instrs` (all 11) | ✅ pass, on the DMG and on the CGB |
| Blargg `instr_timing` | ✅ pass, on both |
| Blargg `mem_timing` and `mem_timing-2` | ✅ pass, on both |
| Blargg `halt_bug` | ✅ pass |
| Blargg `dmg_sound` (all 12) | ✅ pass |
| Blargg `cgb_sound` (all 12) | ✅ pass |
| A4 tone on each melodic channel ([tests/tone-test.py](tests/tone-test.py)) | ✅ 439.8 Hz measured in the recorded WAV, as the hardware formula predicts |
| [dmg-acid2](https://github.com/mattcurrie/dmg-acid2) (PPU) | ✅ pixel-identical to the reference, natively and in the WebAssembly build |
| [cgb-acid2](https://github.com/mattcurrie/cgb-acid2) (CGB PPU) | ✅ pixel-identical to the reference, natively and in the WebAssembly build |
| [mooneye test suite](https://github.com/Gekkio/mooneye-test-suite): every `acceptance` and `emulator-only` test that applies to the DMG (94) | ✅ all pass: CPU and interrupt timing, timer, OAM DMA, PPU timing, serial, boot state, MBC1/2/5 |
| mooneye on a CGB in compatibility mode: every test that applies to the CGB (86, including the CGB-only `misc` boot and PPU tests) | ✅ all pass |
| mooneye `manual-only/sprite_priority` | ✅ pixel-identical to the reference |
| [mealybug tearoom tests](https://github.com/mattcurrie/mealybug-tearoom-tests) (registers changed mid-line, DMG) | ⚠️ 3 of 24 pixel-identical (`m3_bgp_change`, `m3_scx_low_3_bits`, `m2_win_en_toggle`); in the rest most pixels match, with differences in the exact dot where some fetches, window restarts and sprite stalls happen |
| Blargg `oam_bug` (combined ROM, all 8) | ✅ pass |

The mooneye tests for other models (SGB, MGB, AGB and the first DMG and CGB revisions) are skipped. The ROMs are not in
the repository (Blargg's carry no licence): `tests/fetch-roms.sh` downloads them from pinned releases and checks their
SHA-256.

## Play

Open the [web version](https://andreaacanfora.github.io/mattone/). It starts with *Tobu Tobu Girl Deluxe*, a free
homebrew game in colour; **📂 Load ROM** (or drag and drop) runs any `.gb` or `.gbc` file from your computer. Nothing is
uploaded: the file goes straight into the WebAssembly module's memory. **Auto** runs colour games on a Game Boy Color
and the others on the original; you can also force either machine.

| Game Boy | Keyboard |
| --- | --- |
| D-pad | arrow keys |
| A / B | `X` / `Z` |
| Start / Select | `Enter` / `Shift` |
| fast forward | hold `Space` |
| sound on/off | `M` |
| pause / reset | `P` / `R` |

Sound starts with your first click or key press (browsers require one). On a phone, use the on-screen buttons (you can slide your thumb across the D-pad). Games with a battery save to your
browser's local storage. Original Game Boy games come in three colour schemes: grey, DMG green and Pocket.

## Build

The core (`src/cpu.c`, `bus.c`, `ppu.c`, `apu.c`, `cart.c`) has no dependencies at all: no libc, no allocation. The caller owns
the `gb_t` and the ROM buffer.

```sh
make                      # build/mattone-run, the headless runner
make test                 # fetch the test ROMs and run them
WASI_SDK=/path/to/wasi-sdk scripts/build-web.sh   # web/mattone.wasm
python3 -m http.server -d web                      # then open http://localhost:8000
```

Any Clang with the `wasm32` target and `wasm-ld` works too (`CLANG=clang scripts/build-web.sh`). The module is built
freestanding with `-nostdlib` and has **zero imports**: JavaScript copies the ROM into its memory, calls
`run_frame()` and reads the RGBA screen and the 16-bit stereo samples straight out of linear memory.

### The headless runner

```sh
build/mattone-run --serial --until Passed cpu_instrs.gb        # print what a test ROM reports
build/mattone-run --frames 120 --screenshot out.pgm game.gb    # save the screen after 2 seconds (PPM for CGB games)
build/mattone-run --dmg game.gbc                               # force a machine: --dmg or --cgb
build/mattone-run --input moves.txt --dump frames/ game.gb     # scripted buttons, every frame to disk
build/mattone-run --frames 1800 --wav music.wav game.gb       # record 30 seconds of sound
build/mattone-run --mooneye acceptance/div_timing.gb          # pass/fail of a mooneye test ROM
```

### Using the core from C

```c
#include "gb.h"

static gb_t gb;                     /* about 250 KB, so not on the stack */
gb_load(&gb, rom, rom_size);        /* GB_OK, or GB_ERR_SIZE / GB_ERR_CART / GB_ERR_HEADER;
                                       gb_load_model() picks the DMG or the CGB yourself */
gb_set_sample_rate(&gb, 48000);     /* optional: sound costs about 15% more time */
for (;;) {
    gb_set_buttons(&gb, GB_A | GB_RIGHT);
    gb_run_frame(&gb);              /* 70224 cycles */
    draw(gb.pixels);                /* 144 rows of 160: DMG shades 0-3, or RGB555 if gb.cgb */
    int16_t samples[GB_AUDIO_MAX * 2];
    play(samples, gb_audio_take(&gb, samples, GB_AUDIO_MAX));  /* ~800 stereo frames */
}
```

## Speed

6000 frames of Tobu Tobu Girl (DMG) and Tobu Tobu Girl Deluxe (CGB), on a 4-core ARM64 cloud server:

| Build | Without sound | With sound (48 kHz) |
| --- | --- | --- |
| native, GCC 13 `-O2` | ~990 fps DMG, ~1,060 fps CGB (~16-17× real time) | ~900 fps DMG, ~940 fps CGB (~15×) |
| WebAssembly in Node 24 | ~630 fps (~10×) | ~580 fps (~9×) |

## Layout

```
src/gb.h         public API
src/cpu.c        SM83 instructions and interrupt dispatch
src/bus.c        memory map, I/O registers, timer, OAM DMA, the clock
src/ppu.c        pixel FIFO renderer (DMG and CGB), LCD timing, OAM corruption bug
src/apu.c        the four sound channels, frame sequencer and mixer
src/cart.c       memory bank controllers and the MBC3 clock
src/cli.c        mattone-run
src/wasm.c       WebAssembly exports
web/             the browser front end (plain HTML, CSS and JS, no build step)
tests/           test runner, ROM fetcher, screenshot comparison
```

## Support

mattone is free and open source. If it brought back a few minutes of 1989, you can
[buy me a coffee on PayPal](https://www.paypal.com/paypalme/AndreaAca). Thank you!

## License

[MIT](LICENSE) © Andrea Acanfora. The bundled demo game, *Tobu Tobu Girl* by Tangram Games, is MIT (code) and
CC BY 4.0 (assets): see [web/roms/LICENSE-tobutobugirl.txt](web/roms/LICENSE-tobutobugirl.txt).
