<div align="center">

# mattone

**A Game Boy emulator written in C11, with sound. The same core runs natively and in your browser as a 21 KB WebAssembly module.**

*Mattone* is Italian for "brick", the nickname the original Game Boy earned.

### [▶ Play it in your browser](https://andreaacanfora.github.io/mattone/)

[![CI](https://github.com/AndreaAcanfora/mattone/actions/workflows/ci.yml/badge.svg)](https://github.com/AndreaAcanfora/mattone/actions/workflows/ci.yml)
[![C11](https://img.shields.io/badge/C-11-A8B9CC?logo=c&logoColor=white)](src)
[![WebAssembly](https://img.shields.io/badge/wasm-21_KB-654FF0?logo=webassembly&logoColor=white)](src/wasm.c)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Donate with PayPal](https://img.shields.io/badge/donate-PayPal-00457C?logo=paypal)](https://www.paypal.com/paypalme/AndreaAca)

![mattone running Tobu Tobu Girl: the title screen, the level menu, then the girl bouncing off bats as she climbs](docs/demo.gif)

<sub>Recorded from the emulator itself (`mattone-run --dump`, then [scripts/make-gif.py](scripts/make-gif.py)).
Game: [Tobu Tobu Girl](https://github.com/SimonLarsen/tobutobugirl) by Tangram Games.</sub>

</div>

## What it emulates

| Part | Status |
| --- | --- |
| **CPU** (Sharp SM83) | every opcode including the CB prefix, cycle-accurate per memory access, interrupts, `HALT` and the HALT bug, `EI` delay |
| **Timer** | DIV/TIMA/TMA/TAC with the falling-edge behaviour and the one-cycle overflow delay |
| **PPU** | background, window (with its own line counter), 8×8 and 8×16 sprites, the 10-per-line limit, DMG priority rules, STAT interrupts, scanline timing |
| **OAM DMA** | 160 cycles, one byte per cycle |
| **Cartridges** | ROM only, MBC1, MBC2, MBC3 with the real-time clock, MBC5; battery saves |
| **Joypad, serial** | buttons with the joypad interrupt; the link port with nothing plugged in |
| **Sound** | both square channels (with sweep), wave and noise; length, envelope and sweep units on the 512 Hz frame sequencer; DMG quirks such as wave RAM access while playing; stereo panning and master volume; a high-pass filter like the real output capacitor |
| **Game Boy Color** | no: original DMG only |

## Accuracy

The test ROMs run on every push, on GCC and Clang, and again under AddressSanitizer and UBSan:

| Test ROM | Result |
| --- | --- |
| Blargg `cpu_instrs` (all 11) | ✅ pass |
| Blargg `instr_timing` | ✅ pass |
| Blargg `mem_timing` and `mem_timing-2` | ✅ pass |
| Blargg `halt_bug` | ✅ pass |
| Blargg `dmg_sound` (all 12) | ✅ pass |
| A4 tone on each melodic channel ([tests/tone-test.py](tests/tone-test.py)) | ✅ 439.8 Hz measured in the recorded WAV, as the hardware formula predicts |
| [dmg-acid2](https://github.com/mattcurrie/dmg-acid2) (PPU) | ✅ pixel-identical to the reference, natively and in the WebAssembly build |
| Blargg `oam_bug` | ❌ the OAM corruption bug is not emulated |

The ROMs are not in the repository (Blargg's carry no licence): `tests/fetch-roms.sh` downloads them from pinned
commits and checks their SHA-256.

## Play

Open the [web version](https://andreaacanfora.github.io/mattone/). It starts with *Tobu Tobu Girl*, a free
homebrew game; **📂 Load ROM** (or drag and drop) runs any `.gb` file from your computer. Nothing is uploaded: the file
goes straight into the WebAssembly module's memory.

| Game Boy | Keyboard |
| --- | --- |
| D-pad | arrow keys |
| A / B | `X` / `Z` |
| Start / Select | `Enter` / `Shift` |
| fast forward | hold `Space` |
| sound on/off | `M` |
| pause / reset | `P` / `R` |

Sound starts with your first click or key press (browsers require one). On a phone, use the on-screen buttons (you can slide your thumb across the D-pad). Games with a battery save to your
browser's local storage. Three colour schemes: grey, DMG green and Pocket.

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
build/mattone-run --frames 120 --screenshot out.pgm game.gb    # save the screen after 2 seconds
build/mattone-run --input moves.txt --dump frames/ game.gb     # scripted buttons, every frame to disk
build/mattone-run --frames 1800 --wav music.wav game.gb       # record 30 seconds of sound
```

### Using the core from C

```c
#include "gb.h"

static gb_t gb;                     /* about 175 KB, so not on the stack */
gb_load(&gb, rom, rom_size);        /* GB_OK, or GB_ERR_SIZE / GB_ERR_CART / GB_ERR_HEADER */
gb_set_sample_rate(&gb, 48000);     /* optional: sound costs about 15% more time */
for (;;) {
    gb_set_buttons(&gb, GB_A | GB_RIGHT);
    gb_run_frame(&gb);              /* 70224 cycles */
    draw(gb.pixels);                /* 144 rows of 160 shades, 0 = white .. 3 = black */
    int16_t samples[GB_AUDIO_MAX * 2];
    play(samples, gb_audio_take(&gb, samples, GB_AUDIO_MAX));  /* ~800 stereo frames */
}
```

## Speed

6000 frames of Tobu Tobu Girl, on a 4-core ARM64 cloud server:

| Build | Without sound | With sound (48 kHz) |
| --- | --- | --- |
| native, GCC 13 `-O2` | ~1,900 fps (~32× real time) | ~1,640 fps (~27×) |
| WebAssembly in Node 24 | ~1,140 fps (~19×) | ~1,010 fps (~17×) |

## Layout

```
src/gb.h         public API
src/cpu.c        SM83 instructions and interrupt dispatch
src/bus.c        memory map, I/O registers, timer, OAM DMA, the clock
src/ppu.c        scanline renderer and LCD timing
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
