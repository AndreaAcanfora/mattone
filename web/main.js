// Browser glue: input, timing, saves. The whole Game Boy runs in C (mattone.wasm).

const FRAME_MS = 1000 / 59.7275; // the DMG refreshes at 4194304 / 70224 Hz
const TURBO = 4;
const DEMO = { url: "roms/tobutobugirldx.gb", name: "Tobu Tobu Girl Deluxe" };
const BUTTONS = { right: 1, left: 2, up: 4, down: 8, a: 16, b: 32, select: 64, start: 128 };
const KEYS = {
  ArrowRight: "right", ArrowLeft: "left", ArrowUp: "up", ArrowDown: "down",
  KeyX: "a", KeyZ: "b", Enter: "start", ShiftLeft: "select", ShiftRight: "select", Backspace: "select",
};
const PALETTES = {
  grey: ["#ffffff", "#aaaaaa", "#555555", "#000000"],
  dmg: ["#9bbc0f", "#8bac0f", "#306230", "#0f380f"],
  pocket: ["#c4cfa1", "#8b956d", "#4d533c", "#1f1f1f"],
};

const canvas = document.getElementById("screen");
const ctx = canvas.getContext("2d");
const stats = document.getElementById("stats");
const titleEl = document.getElementById("title");
const toast = document.getElementById("toast");

const { instance } = await loadWasm("mattone.wasm");
const wasm = instance.exports;

let romBytes = null; // the cartridge currently inserted
let saveKey = null;
let savedCopy = null;
let paused = false;
let turbo = false;
let held = 0; // buttons from keyboard and touch
let audio = null; // { ctx, next } once the browser lets us play sound
let muted = localStorage.getItem("mattone:muted") === "1";
const pointerButtons = new Map(); // pointer id -> names of the buttons under it
const touchHeld = new Set(); // buttons held by any pointer

async function loadWasm(url) {
  if (WebAssembly.instantiateStreaming) {
    try {
      return await WebAssembly.instantiateStreaming(fetch(url), {});
    } catch {
      // Some servers send the wrong MIME type: fall back to bytes.
    }
  }
  const bytes = await (await fetch(url)).arrayBuffer();
  return WebAssembly.instantiate(bytes, {});
}

function mem() {
  return new Uint8Array(wasm.memory.buffer);
}

function showToast(text) {
  toast.textContent = text;
  toast.classList.add("show");
  clearTimeout(showToast.timer);
  showToast.timer = setTimeout(() => toast.classList.remove("show"), 2500);
}

// ---------- cartridges and saves ----------

function insert(bytes, label) {
  if (bytes.length > wasm.rom_capacity()) {
    showToast("That file is too big for a Game Boy ROM");
    return false;
  }
  persistSave();
  mem().set(bytes, wasm.rom_buffer());
  const err = wasm.load(bytes.length, Number(modelSelect.value));
  if (err !== 0) {
    showToast(err === -2 ? "Unsupported cartridge type" : "That doesn't look like a Game Boy ROM");
    if (romBytes) insert(romBytes, titleEl.textContent); // put the previous game back
    return false;
  }
  romBytes = bytes;
  applySampleRate();
  const title = readTitle() || label;
  titleEl.textContent = title;
  document.title = `${title} · mattone`;
  if (bytes[0x143] === 0xc0 && !wasm.is_cgb()) showToast("This game needs a Game Boy Color: choose Auto or Color");
  paletteSelect.disabled = Boolean(wasm.is_cgb());
  restoreSave(bytes, title);
  return true;
}

function readTitle() {
  const m = mem();
  let s = "";
  for (let p = wasm.title(); m[p]; p++) s += String.fromCharCode(m[p]);
  return s.split(/\s{2,}/)[0].trim(); // some headers pad the title with spaces and a maker code
}

function restoreSave(bytes, title) {
  const size = wasm.save_size();
  if (!size) {
    saveKey = null;
    return;
  }
  // The header's global checksum tells apart ROMs with the same title.
  const checksum = ((bytes[0x14e] << 8) | bytes[0x14f]).toString(16).padStart(4, "0");
  saveKey = `mattone:save:${title}:${checksum}`;
  const stored = localStorage.getItem(saveKey);
  if (stored) {
    const data = Uint8Array.from(atob(stored), (c) => c.charCodeAt(0));
    mem().set(data.subarray(0, size), wasm.save_ram());
  }
  savedCopy = mem().slice(wasm.save_ram(), wasm.save_ram() + size);
}

function persistSave() {
  if (!saveKey) return;
  const size = wasm.save_size();
  const ram = mem().subarray(wasm.save_ram(), wasm.save_ram() + size);
  if (savedCopy && ram.every((v, i) => v === savedCopy[i])) return;
  let s = "";
  for (const v of ram) s += String.fromCharCode(v);
  try {
    localStorage.setItem(saveKey, btoa(s));
    savedCopy = ram.slice();
  } catch {
    showToast("Could not save: browser storage is full");
  }
}

setInterval(persistSave, 2000);
addEventListener("pagehide", persistSave);
document.addEventListener("visibilitychange", () => document.hidden && persistSave());

async function playDemo() {
  const res = await fetch(DEMO.url);
  insert(new Uint8Array(await res.arrayBuffer()), DEMO.name);
}

async function openFile(file) {
  if (!file) return;
  if (insert(new Uint8Array(await file.arrayBuffer()), file.name.replace(/\.[^.]*$/, ""))) {
    showToast(`Loaded ${file.name}`);
  }
}

// ---------- sound ----------

// Browsers only allow audio after a click or key press.
function startAudio() {
  if (audio || muted) return;
  const ctx = new AudioContext({ latencyHint: "interactive" });
  audio = { ctx, next: 0 };
  applySampleRate();
}

function applySampleRate() {
  wasm.set_sample_rate(audio && !muted ? audio.ctx.sampleRate : 0);
}

function playAudio() {
  const frames = wasm.audio_take();
  if (!frames || !audio || turbo) return;
  const { ctx } = audio;
  const pcm = new Int16Array(wasm.memory.buffer, wasm.audio_buffer(), frames * 2);
  const buffer = ctx.createBuffer(2, frames, ctx.sampleRate);
  const left = buffer.getChannelData(0);
  const right = buffer.getChannelData(1);
  for (let i = 0; i < frames; i++) {
    left[i] = pcm[i * 2] / 32768;
    right[i] = pcm[i * 2 + 1] / 32768;
  }
  // Queue the chunks back to back, restarting the queue if it ran dry or
  // drifted too far ahead of the clock.
  const now = ctx.currentTime;
  if (audio.next < now + 0.01 || audio.next > now + 0.25) audio.next = now + 0.05;
  const source = ctx.createBufferSource();
  source.buffer = buffer;
  source.connect(ctx.destination);
  source.start(audio.next);
  audio.next += frames / ctx.sampleRate;
}

function setMuted(on) {
  muted = on;
  localStorage.setItem("mattone:muted", on ? "1" : "0");
  soundBtn.textContent = on ? "🔇" : "🔊";
  soundBtn.classList.toggle("active", on);
  if (!on) startAudio();
  if (audio) on ? audio.ctx.suspend() : audio.ctx.resume();
  applySampleRate();
}

for (const type of ["pointerdown", "keydown"]) {
  addEventListener(type, () => audio ? audio.ctx.resume() : startAudio(), { capture: true });
}

// ---------- screen ----------

function setPalette(name) {
  const colors = PALETTES[name] ?? PALETTES.grey;
  const view = new DataView(wasm.memory.buffer, wasm.palette(), 16);
  colors.forEach((hex, i) => {
    const n = parseInt(hex.slice(1), 16);
    // RGBA bytes in memory = little-endian 0xAABBGGRR.
    view.setUint32(i * 4, (0xff << 24) | ((n & 0xff) << 16) | (n & 0xff00) | (n >> 16), true);
  });
  localStorage.setItem("mattone:palette", name);
  wasm.render();
  draw();
}

let image = null;
function draw() {
  if (!image || image.data.buffer !== wasm.memory.buffer) {
    image = new ImageData(new Uint8ClampedArray(wasm.memory.buffer, wasm.screen(), 160 * 144 * 4), 160, 144);
  }
  ctx.putImageData(image, 0, 0);
}

// ---------- main loop ----------

let last = performance.now();
let debt = 0;
let fpsFrames = 0;
let fpsStart = last;

function loop(now) {
  const elapsed = Math.min(now - last, 250); // don't race to catch up after a hidden tab
  last = now;
  if (!paused && romBytes) {
    debt += elapsed * (turbo ? TURBO : 1);
    let ran = 0;
    while (debt >= FRAME_MS && ran < TURBO * 2) {
      wasm.set_buttons(held);
      wasm.run_frame();
      debt -= FRAME_MS;
      ran++;
    }
    if (ran) {
      draw();
      playAudio();
    }
    fpsFrames += ran;
  }
  if (now - fpsStart >= 1000) {
    stats.textContent = paused ? "paused" : `${Math.round((fpsFrames * 1000) / (now - fpsStart))} fps`;
    fpsFrames = 0;
    fpsStart = now;
  }
  requestAnimationFrame(loop);
}

// ---------- input ----------

function press(name, down) {
  const bit = BUTTONS[name];
  held = down ? held | bit : held & ~bit;
  document.querySelectorAll(`[data-button="${name}"]`).forEach((b) => b.classList.toggle("held", down));
}

addEventListener("keydown", (e) => {
  if (e.target.closest?.("select")) return;
  const name = KEYS[e.code];
  if (name) {
    e.preventDefault();
    press(name, true);
  } else if (e.code === "Space") {
    e.preventDefault();
    setTurbo(true);
  } else if (e.code === "KeyM" && !e.repeat) {
    setMuted(!muted);
  } else if (e.code === "KeyP" && !e.repeat) {
    togglePause();
  } else if (e.code === "KeyR" && !e.repeat && romBytes) {
    insert(romBytes, titleEl.textContent);
  }
});
addEventListener("keyup", (e) => {
  const name = KEYS[e.code];
  if (name) press(name, false);
  else if (e.code === "Space") setTurbo(false);
});
addEventListener("blur", () => {
  for (const name of Object.keys(BUTTONS)) press(name, false);
  pointerButtons.clear();
  touchHeld.clear();
  setTurbo(false);
});

// Touch and mouse: a finger can slide from one button to the next. The d-pad is
// read by angle from its centre over an area wider than the drawn cross, so a
// thumb that lands on a corner or drifts off an arm still steers, and the
// corners give diagonals.
const pad = document.getElementById("pad");
const dpad = document.getElementById("dpad");
const DPAD_REACH = 1.4; // touch area, as a multiple of the drawn d-pad
const DPAD_DEAD = 0.18; // centre dead zone, as a fraction of the half width
const DPAD_SECTORS = [["right"], ["right", "down"], ["down"], ["down", "left"],
  ["left"], ["left", "up"], ["up"], ["up", "right"]];
function dpadOffset(x, y) {
  const r = dpad.getBoundingClientRect();
  const half = r.width / 2;
  const dx = x - (r.left + half);
  const dy = y - (r.top + r.height / 2);
  const inside = Math.abs(dx) <= half * DPAD_REACH && Math.abs(dy) <= half * DPAD_REACH;
  return inside ? { dx, dy, half } : null;
}
function buttonsAt(x, y) {
  const d = dpadOffset(x, y);
  if (d) {
    const { dx, dy, half } = d;
    if (Math.hypot(dx, dy) < half * DPAD_DEAD) return [];
    // 8 sectors of 45°, with diagonals; screen y grows downwards.
    const sector = Math.round(Math.atan2(dy, dx) / (Math.PI / 4));
    return DPAD_SECTORS[(sector + 8) % 8];
  }
  const name = document.elementFromPoint(x, y)?.closest?.("[data-button]")?.dataset.button;
  return name ? [name] : [];
}
function syncPointers() {
  const now = new Set([...pointerButtons.values()].flat());
  for (const name of touchHeld) if (!now.has(name)) press(name, false);
  for (const name of now) if (!touchHeld.has(name)) press(name, true);
  const grew = [...now].some((name) => !touchHeld.has(name));
  touchHeld.clear();
  now.forEach((name) => touchHeld.add(name));
  if (grew) navigator.vibrate?.(8);
}
pad.addEventListener("pointerdown", (e) => {
  const names = buttonsAt(e.clientX, e.clientY);
  if (!names.length && !dpadOffset(e.clientX, e.clientY)) return;
  e.preventDefault();
  pad.setPointerCapture(e.pointerId);
  pointerButtons.set(e.pointerId, names);
  syncPointers();
});
pad.addEventListener("pointermove", (e) => {
  if (!pointerButtons.has(e.pointerId)) return;
  pointerButtons.set(e.pointerId, buttonsAt(e.clientX, e.clientY));
  syncPointers();
});
for (const type of ["pointerup", "pointercancel"]) {
  pad.addEventListener(type, (e) => {
    pointerButtons.delete(e.pointerId);
    syncPointers();
  });
}
pad.addEventListener("contextmenu", (e) => e.preventDefault());

// ---------- toolbar ----------

const pauseBtn = document.getElementById("pause");
const turboBtn = document.getElementById("turbo");
const soundBtn = document.getElementById("sound");

function togglePause() {
  paused = !paused;
  pauseBtn.textContent = paused ? "▶" : "❚❚";
  pauseBtn.classList.toggle("active", paused);
  if (paused) persistSave();
}

function setTurbo(on) {
  turbo = on;
  turboBtn.classList.toggle("active", on);
}

pauseBtn.addEventListener("click", togglePause);
soundBtn.addEventListener("click", () => setMuted(!muted));
soundBtn.textContent = muted ? "🔇" : "🔊";
soundBtn.classList.toggle("active", muted);
turboBtn.addEventListener("click", () => setTurbo(!turbo));
document.getElementById("reset").addEventListener("click", () => romBytes && insert(romBytes, titleEl.textContent));
document.getElementById("demo").addEventListener("click", playDemo);
document.getElementById("file").addEventListener("change", (e) => openFile(e.target.files[0]));

const paletteSelect = document.getElementById("palette");
const modelSelect = document.getElementById("model");
modelSelect.value = localStorage.getItem("mattone:model") ?? "0";
modelSelect.addEventListener("change", () => {
  localStorage.setItem("mattone:model", modelSelect.value);
  modelSelect.blur();
  if (romBytes) insert(romBytes, titleEl.textContent); // restart on the chosen machine
});
paletteSelect.value = localStorage.getItem("mattone:palette") ?? "grey";
paletteSelect.addEventListener("change", () => {
  setPalette(paletteSelect.value);
  paletteSelect.blur();
});
setPalette(paletteSelect.value);

// Drag and drop a ROM anywhere on the page.
addEventListener("dragover", (e) => {
  e.preventDefault();
  document.body.classList.add("dragging");
});
addEventListener("dragleave", (e) => {
  if (!e.relatedTarget) document.body.classList.remove("dragging");
});
addEventListener("drop", (e) => {
  e.preventDefault();
  document.body.classList.remove("dragging");
  openFile(e.dataTransfer.files[0]);
});

await playDemo();
requestAnimationFrame(loop);
