// Browser glue: input, timing, saves. The whole Game Boy runs in C (mattone.wasm).

const FRAME_MS = 1000 / 59.7275; // the DMG refreshes at 4194304 / 70224 Hz
const TURBO = 4;
const DEMO = { url: "roms/tobutobugirl.gb", name: "Tobu Tobu Girl" };
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
const pointerButtons = new Map();

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
  const err = wasm.load(bytes.length);
  if (err !== 0) {
    showToast(err === -2 ? "Unsupported cartridge type" : "That doesn't look like a Game Boy ROM");
    if (romBytes) insert(romBytes, titleEl.textContent); // put the previous game back
    return false;
  }
  romBytes = bytes;
  const title = readTitle() || label;
  titleEl.textContent = title;
  document.title = `${title} · mattone`;
  if (bytes[0x143] === 0xc0) showToast("Game Boy Color only: it may not run");
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
    if (ran) draw();
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
  setTurbo(false);
});

// Touch and mouse: a finger can slide from one button to the next.
const pad = document.getElementById("pad");
function buttonAt(x, y) {
  return document.elementFromPoint(x, y)?.closest?.("[data-button]")?.dataset.button ?? null;
}
function pointerMove(e) {
  if (!pointerButtons.has(e.pointerId)) return;
  const before = pointerButtons.get(e.pointerId);
  const now = buttonAt(e.clientX, e.clientY);
  if (now === before) return;
  if (before) press(before, false);
  if (now) press(now, true);
  pointerButtons.set(e.pointerId, now);
}
pad.addEventListener("pointerdown", (e) => {
  const name = buttonAt(e.clientX, e.clientY);
  if (!name) return;
  e.preventDefault();
  pad.setPointerCapture(e.pointerId);
  pointerButtons.set(e.pointerId, name);
  press(name, true);
  navigator.vibrate?.(8);
});
pad.addEventListener("pointermove", pointerMove);
for (const type of ["pointerup", "pointercancel"]) {
  pad.addEventListener(type, (e) => {
    const name = pointerButtons.get(e.pointerId);
    pointerButtons.delete(e.pointerId);
    if (name && ![...pointerButtons.values()].includes(name)) press(name, false);
  });
}
pad.addEventListener("contextmenu", (e) => e.preventDefault());

// ---------- toolbar ----------

const pauseBtn = document.getElementById("pause");
const turboBtn = document.getElementById("turbo");

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
turboBtn.addEventListener("click", () => setTurbo(!turbo));
document.getElementById("reset").addEventListener("click", () => romBytes && insert(romBytes, titleEl.textContent));
document.getElementById("demo").addEventListener("click", playDemo);
document.getElementById("file").addEventListener("change", (e) => openFile(e.target.files[0]));

const paletteSelect = document.getElementById("palette");
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
