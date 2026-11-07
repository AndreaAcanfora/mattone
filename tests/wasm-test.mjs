// Runs dmg-acid2 inside web/mattone.wasm and writes the screen as a PGM, so
// tests/compare.py can check the browser build against the reference image.
// Usage: node tests/wasm-test.mjs ROM OUT.pgm
import { readFileSync, writeFileSync } from "node:fs";

const [rom, out] = process.argv.slice(2);
const module = new WebAssembly.Module(readFileSync(new URL("../web/mattone.wasm", import.meta.url)));
const imports = WebAssembly.Module.imports(module);
if (imports.length) throw new Error(`the module should need no imports, has ${JSON.stringify(imports)}`);

const wasm = new WebAssembly.Instance(module, {}).exports;
const bytes = readFileSync(rom);
new Uint8Array(wasm.memory.buffer).set(bytes, wasm.rom_buffer());
const err = wasm.load(bytes.length);
if (err !== 0) throw new Error(`load failed: ${err}`);
for (let i = 0; i < 60; i++) wasm.run_frame();

// The default palette is grey, so the red channel is the shade.
const rgba = new Uint8Array(wasm.memory.buffer, wasm.screen(), 160 * 144 * 4);
const grey = Buffer.alloc(160 * 144);
for (let i = 0; i < grey.length; i++) grey[i] = rgba[i * 4];
writeFileSync(out, Buffer.concat([Buffer.from("P5\n160 144\n255\n"), grey]));
