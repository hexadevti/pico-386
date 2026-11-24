// pico-386 -- the browser shell (emu8's web techniques). It owns the page: the layout (portrait /
// landscape beside / landscape over), the PC's picture at its own resolution and the 320x320 panel
// read out of the wasm memory, the emulator's menus drawn as touch screens (ui.cpp sends them as
// JSON, a tap answers with the key it stands for), the physical
// keyboard and an on-screen PC keyboard (both as PicoCalc key codes, so pc_keymap does the rest
// exactly as on the device), the canvas as a touchpad (pointer lock with a real mouse) and a
// trackpad (🖱) in the keyboard's place, the
// IndexedDB-backed /sdcard with the files a new visitor starts with, uploads, "receive from PC"
// and the audio (an AudioWorklet draining hal_web.cpp's ring through the SharedArrayBuffer).
'use strict';

const $ = (s) => document.querySelector(s);
const app = $('#app'), canvas = $('#screen'), ctx = canvas.getContext('2d', { alpha: false });
const bootMsg = $('#boot-msg');
let M = null;                 // the Emscripten module once running

const FOLDERS = ['pico-386', 'pico-386/disks', 'pico-386/games', 'pico-386/roms'];

// PicoCalc key codes (pico386/src/hal/hal.h)
const K = {
  BS: 0x08, TAB: 0x09, ENTER: 0x0a, ESC: 0xb1, LEFT: 0xb4, UP: 0xb5, DOWN: 0xb6, RIGHT: 0xb7,
  ALT: 0xa1, SHIFT_L: 0xa2, SHIFT_R: 0xa3, CTRL: 0xa5, CAPS: 0xc1,
  BREAK: 0xd0, INS: 0xd1, HOME: 0xd2, DEL: 0xd4, END: 0xd5, PGUP: 0xd6, PGDN: 0xd7,
  F: [0, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x90],
  RELEASE_ALL: 0x00,
};

// The machine a new visitor gets: the free FreeDOS disk the repo ships (seeded below).
const DEFAULT_INI = `; pico-386 PC. tiny386's .ini keys; paths are relative to the SD card root.
[pc]
mem_size = 64M
vga_mem_size = 1M
hda = pico-386/disks/freedos-hdd-32mb.img
fill_cmos = 1

[cpu]
gen = 4
fpu = 1

[pico]
name = FreeDOS
mouse = 0
`;

if (!self.crossOriginIsolated) {
  bootMsg.removeAttribute('data-i18n');
  bootMsg.textContent = t('boot.noSAB');
}

// ======================= persistence (IDBFS) =======================
let syncBusy = false, syncAgain = false, syncTimer = 0;
function persist(cb) {
  if (!M) return cb && cb();
  if (syncBusy) { syncAgain = true; if (cb) setTimeout(() => persist(cb), 200); return; }
  syncBusy = true;
  M.FS.syncfs(false, (err) => {
    syncBusy = false;
    if (err) console.warn('syncfs', err);
    if (syncAgain) { syncAgain = false; schedulePersist(); }
    cb && cb();
  });
}
function schedulePersist() { clearTimeout(syncTimer); syncTimer = setTimeout(() => persist(), 400); }

function exists(FS, p) { try { FS.stat(p); return true; } catch (e) { return false; } }
function mkdirs(FS, path) {
  const parts = path.split('/'); let d = '';
  for (let i = 1; i < parts.length; i++) { d += '/' + parts[i]; try { FS.mkdir(d); } catch (e) {} }
}

// ======================= module =======================
const moduleCfg = {
  print: (s) => console.log(s),
  printErr: (s) => console.warn(s),
  // hal_reboot / hal_reboot_to_loader (home menu Restart, Ctrl+PgUp, Ctrl+F8): save, then reload
  webReboot: () => persist(() => location.reload()),
  preRun: [(mod) => {
    const FS = mod.FS, IDBFS = mod.IDBFS || (FS.filesystems && FS.filesystems.IDBFS);
    try { FS.mkdir('/sdcard'); } catch (e) {}
    FS.mount(IDBFS, {}, '/sdcard');
    mod.addRunDependency('idbfs');
    FS.syncfs(true, async (err) => {
      if (err) console.warn('syncfs load', err);
      for (const f of FOLDERS) mkdirs(FS, '/sdcard/' + f);
      try {
        if (/^(localhost|127\.0\.0\.1)$/.test(location.hostname)) await devPreload(FS);
        let n = await seed(FS);
        if (!exists(FS, '/sdcard/pico-386/pc.ini')) FS.writeFile('/sdcard/pico-386/pc.ini', DEFAULT_INI);
        else {
          // pc.ini seeded before 64 MB was the web's default: raise its old 32 MB once.
          const ini = FS.readFile('/sdcard/pico-386/pc.ini', { encoding: 'utf8' });
          const old = /^mem_size *= *32M\b/m;
          if (old.test(ini)) {
            FS.writeFile('/sdcard/pico-386/pc.ini', ini.replace(old, 'mem_size = 64M'));
            n++;
          }
        }
        if (n) await new Promise((res) => FS.syncfs(false, () => res()));
      } catch (e) { console.warn('seed', e); }
      mod.removeRunDependency('idbfs');
    });
  }],
};

// The files a new visitor starts with (web/build.py writes seed/index.json): the BIOS, and the
// FreeDOS disk gzipped. Copied only where missing, so the visitor's own changes stay.
async function seed(FS) {
  let list;
  try { const r = await fetch('seed/index.json', { cache: 'no-cache' }); if (!r.ok) return 0; list = await r.json(); } catch (e) { return 0; }
  let n = 0;
  for (const f of list) {
    const dst = '/sdcard/' + f.path;
    if (exists(FS, dst)) continue;
    bootMsg.removeAttribute('data-i18n');
    bootMsg.textContent = t('boot.seeding', f.path.split('/').pop());
    const r = await fetch(f.url);
    if (!r.ok) { console.warn('seed', f.url, r.status); continue; }
    const body = f.gz ? r.body.pipeThrough(new DecompressionStream('gzip')) : r.body;
    const data = new Uint8Array(await new Response(body).arrayBuffer());
    if (data.length !== f.size) { console.warn('seed', f.path, 'size', data.length, '!=', f.size); continue; }
    mkdirs(FS, dst.slice(0, dst.lastIndexOf('/')));
    FS.writeFile(dst, data);
    n++;
  }
  return n;
}

// Local test server only (web/serve.py): copy the repo's sdcard/ files that are missing here.
// Never over a file the emulator may have changed (the .ini files, the disks).
async function devPreload(FS) {
  let list;
  try { const r = await fetch('/__sdcard_list'); if (!r.ok) return; list = await r.json(); } catch (e) { return; }
  for (const f of list) {
    const dst = '/sdcard/' + f.path;
    if (exists(FS, dst)) continue;
    bootMsg.removeAttribute('data-i18n');
    bootMsg.textContent = t('boot.copying', f.path);
    const r = await fetch('/__sdcard/' + f.path.split('/').map(encodeURIComponent).join('/'));
    if (!r.ok) continue;
    mkdirs(FS, dst.slice(0, dst.lastIndexOf('/')));
    FS.writeFile(dst, new Uint8Array(await r.arrayBuffer()));
  }
}

createPico386(moduleCfg).then((mod) => {
  M = mod;
  $('#boot').hidden = true;
  requestAnimationFrame(frame);
  // the guest's disk writes, snapshots and settings: saved every 15 s, and when the page hides
  setInterval(() => { if (!document.hidden) persist(); }, 15000);
}).catch((e) => { bootMsg.removeAttribute('data-i18n'); bootMsg.textContent = t('boot.fail', e); });

// Hidden (tab switched, app minimised, phone locked): the emulator and the audio pause, so the phone
// isn't kept busy in the background; the PC carries on where it was (its clock skips the gap).
function setHidden(h) {
  if (M) M._web_hidden(h ? 1 : 0);
  if (actx) { if (h) actx.suspend().catch(() => {}); else actx.resume().catch(() => {}); }
  if (h) releaseAll();
}
document.addEventListener('visibilitychange', () => { if (document.hidden) persist(); setHidden(document.hidden); });
window.addEventListener('pageshow', () => setHidden(document.hidden));
window.addEventListener('pagehide', () => { persist(); setHidden(true); });

// ======================= video =======================
// Two pictures: the PC at its own resolution (pc_video.cpp's pcvid_native: 720x400 text with the
// guest's font, 320x200, 640x480, ...) in a 4:3 box as a monitor shows it, and the 320x320 panel for
// the few screens ui.cpp still draws itself (progress bars, "press a key").
const wrap = $('#screen-wrap'), pcCanvas = $('#pc'), pcCtx = pcCanvas.getContext('2d', { alpha: false });
let lastFrame = -1, lastPcFrame = -1, pcImg = null;
let screenKind = 'panel';             // which picture the screen shows: 'panel' or 'pc'
let uiMode = 'panel';                 // 'panel', 'pc' or 'menu' (#ui over everything)
const img = new ImageData(320, 320);
function frame() {
  requestAnimationFrame(frame);
  if (!M) return;
  pollUi();
  pollStatus();
  const no = M._web_frame_no();
  if (no !== lastFrame) {
    lastFrame = no;
    const ptr = M._web_frame_ptr();
    img.data.set(M.HEAPU8.subarray(ptr, ptr + 320 * 320 * 4));
    ctx.putImageData(img, 0, 0);
  }
  const pno = M._web_pc_frame_no();
  if (pno !== lastPcFrame) {
    lastPcFrame = pno;
    const w = M._web_pc_w(), h = M._web_pc_h();
    if (w > 0 && h > 0) {
      if (!pcImg || pcImg.width !== w || pcImg.height !== h) {
        pcImg = new ImageData(w, h);
        natCanvas.width = w; natCanvas.height = h;
      }
      const ptr = M._web_pc_frame_ptr();
      pcImg.data.set(M.HEAPU8.subarray(ptr, ptr + w * h * 4));
      natCtx.putImageData(pcImg, 0, 0);
      drawPc();
    }
  }
}

// The PC's picture fills the largest 4:3 box the screen area holds (every VGA mode is 4:3 on a
// monitor, 720x400 text and 320x200 too). The guest's picture stays at its own resolution
// (natCanvas) and is scaled "sharp-bilinear": first by whole numbers with no smoothing, to the
// first size at least as big as the box (midCanvas: each guest pixel an even block), then smoothly
// down to the box in device pixels, so only the blocks' edges are blended. Plain nearest-neighbour to a fractional size makes some pixels
// a column wider than others (uneven lines in 320x200); plain bilinear blurs all of them.
const natCanvas = document.createElement('canvas'), natCtx = natCanvas.getContext('2d', { alpha: false });
const midCanvas = document.createElement('canvas'), midCtx = midCanvas.getContext('2d', { alpha: false });
let boxW = 0, boxH = 0;                  // the box in device pixels
// Pinch zoom (two fingers on the screen): the scale, and the view's top-left as a fraction of the
// picture. drawPc scales only the part in view, so a zoomed picture is as sharp as the whole one.
let zoom = 1, zu = 0, zv = 0;
function fitPc() {
  const r = wrap.getBoundingClientRect();
  if (!r.width || !r.height) return;
  let w = r.width, h = w * 3 / 4;
  if (h > r.height) { h = r.height; w = h * 4 / 3; }
  pcCanvas.style.width = w + 'px'; pcCanvas.style.height = h + 'px';
  boxW = Math.max(1, Math.round(w * devicePixelRatio));
  boxH = Math.max(1, Math.round(h * devicePixelRatio));
  drawPc();
}
function drawPc() {
  if (!pcImg || !boxW) return;
  const w = pcImg.width, h = pcImg.height;
  if (pcCanvas.width !== boxW || pcCanvas.height !== boxH) { pcCanvas.width = boxW; pcCanvas.height = boxH; }
  // the guest pixels in view, widened to whole pixels, and how many device pixels each one takes
  const sw = w / zoom, sh = h / zoom, sx = zu * w, sy = zv * h;
  const x0 = Math.floor(sx), y0 = Math.floor(sy);
  const cw = Math.min(w, Math.ceil(sx + sw)) - x0, ch = Math.min(h, Math.ceil(sy + sh)) - y0;
  const px = boxW / sw, py = boxH / sh;
  const kx = Math.max(1, Math.ceil(px - 0.01)), ky = Math.max(1, Math.ceil(py - 0.01));
  let src = natCanvas, sr = [x0, y0, cw, ch];
  if (kx > 1 || ky > 1) {
    if (midCanvas.width !== cw * kx || midCanvas.height !== ch * ky) { midCanvas.width = cw * kx; midCanvas.height = ch * ky; }
    midCtx.imageSmoothingEnabled = false;
    midCtx.drawImage(natCanvas, x0, y0, cw, ch, 0, 0, cw * kx, ch * ky);
    src = midCanvas; sr = [0, 0, cw * kx, ch * ky];
  }
  pcCtx.imageSmoothingEnabled = true;
  pcCtx.imageSmoothingQuality = 'high';
  pcCtx.drawImage(src, ...sr, (x0 - sx) * px, (y0 - sy) * py, cw * px, ch * py);
}
function resetZoom() { if (zoom !== 1) { zoom = 1; zu = zv = 0; drawPc(); } }
new ResizeObserver(fitPc).observe(wrap);

// ---- the status lines over the picture: the 2 s note (mouse mode, zoom) and the FPS meter
const stEl = [$('#st-note'), $('#st-fps')], stNo = [-1, -1], stAt = [0, 0];
function pollStatus() {
  const now = performance.now();
  for (let s = 0; s < 2; s++) {
    const n = M._web_status_no(s);
    if (n !== stNo[s]) {
      stNo[s] = n; stAt[s] = now;
      stEl[s].textContent = M.UTF8ToString(M._web_status(s));
      stEl[s].classList.remove('off');
    } else if (now - stAt[s] > (s ? 3000 : 2500)) stEl[s].classList.add('off');
  }
}

// ======================= menus as touch screens =======================
// ui.cpp publishes each screen as JSON with a sequence number: {"t":"pc"} (a PC runs), {"t":"panel"}
// (it draws on the panel itself), and "list", "msg" and "edit", which this draws. A tap answers
// (seq, key, row) with the key a PicoCalc would have sent there, so the C++ callers run unchanged.
// The JSON is read like a seqlock: a screen replaced while it was read is read again next frame.
const ICONS = { 1: '🎮', 2: '⚙️', 3: '🚪', 4: '📁', 5: '⬆️', 6: '💾', 7: '▶️', 8: '🔄', 9: '💾', 10: '📂',
  11: '🔊', 12: '⏭️', 13: '📊', 14: '⏏️' };
const uiEl = $('#ui'), uiBody = $('#ui-body'), uiBack = $('#ui-back'), uiClose = $('#ui-close');
let uiSeq = 0, uiAnswered = 0, uiKind = '', uiListKey = '', uiTitle = null, uiBackKey = K.ESC;

function pollUi() {
  const s = M._web_ui_seq();
  if (s === uiSeq) return;
  const txt = M.UTF8ToString(M._web_ui_json(s));
  if (M._web_ui_seq() !== s) return;
  let j;
  try { j = JSON.parse(txt); } catch (e) { return; }
  uiSeq = s;
  if (j.t === 'pc' || j.t === 'panel') {
    setMode(j.t);
  } else {
    setMode('menu');
    if (j.t === 'list') renderList(j);
    else if (j.t === 'msg') renderMsg(j);
    else if (j.t === 'edit') renderEdit(j);
  }
}

function uiReply(key, sel = 0) {
  if (!M || uiAnswered === uiSeq) return;   // one answer a screen
  uiAnswered = uiSeq;
  M._web_ui_reply(uiSeq, key, sel);
}

function setMode(m) {
  if (m === 'pc' || m === 'panel') {
    if (screenKind !== m) {
      screenKind = m;
      if (m !== 'pc') resetZoom();
      canvas.hidden = m !== 'panel';
      pcCanvas.hidden = m !== 'pc';
    }
  }
  if (m === uiMode) return;
  uiMode = m;
  uiEl.hidden = m !== 'menu';
  if (m !== 'menu') { uiKind = ''; uiListKey = ''; uiTitle = null; if (document.activeElement && uiEl.contains(document.activeElement)) document.activeElement.blur(); }
  // the touchpad's buttons only while a PC runs
  $('#mbtns').hidden = m !== 'pc';
  if (m !== 'pc' && document.pointerLockElement === wrap) document.exitPointerLock();
  if (m !== 'pc') releaseAll();
  layout();
}

function setHead(title, sub, backKey, backLabel, closeToo) {
  $('#ui-title').textContent = title || '';
  $('#ui-sub').textContent = sub || '';
  uiBack.classList.toggle('hide', !backKey);
  uiBackKey = backKey;
  uiBack.title = backLabel || t('ui.back');
  uiClose.hidden = !closeToo;
}
uiBack.addEventListener('click', () => { if (uiBackKey) uiReply(uiBackKey); });
uiClose.addEventListener('click', () => uiReply(K.ESC));
$('#ui-files').addEventListener('click', () => openFiles());
$('#ui-fs').addEventListener('click', () => toggleFullscreen());

// hints are "Enter|Select;Bs|Up;Esc|Back": which keys the screen takes, and what they mean
function hintOf(hints, cap) {
  for (const h of (hints || '').split(';')) { const [k, l] = h.split('|'); if (k === cap) return l || cap; }
  return null;
}

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined && text !== null) e.textContent = text;
  return e;
}

// a row's picture (a snapshot's screen): RGB565 little-endian, base64
function pictureOf(r) {
  const c = el('canvas', 'ui-pic');
  c.width = r.pw; c.height = r.ph;
  const bin = atob(r.p), im = new ImageData(r.pw, r.ph), d = im.data;
  for (let i = 0, o = 0; i + 1 < bin.length && o < d.length; i += 2, o += 4) {
    const v = bin.charCodeAt(i) | bin.charCodeAt(i + 1) << 8;
    d[o] = (v >> 11) * 255 / 31; d[o + 1] = (v >> 5 & 63) * 255 / 63; d[o + 2] = (v & 31) * 255 / 31; d[o + 3] = 255;
  }
  c.getContext('2d').putImageData(im, 0, 0);
  c.style.height = '48px'; c.style.width = Math.round(48 * r.pw / r.ph) + 'px';
  return c;
}

function renderList(j) {
  const bs = hintOf(j.hints, 'Bs'), esc = hintOf(j.hints, 'Esc');
  // Backspace goes up a folder in the browser; then ✕ leaves it (Esc)
  setHead(j.logo ? '' : j.title, j.logo ? '' : j.subtitle, bs ? K.BS : esc ? K.ESC : 0, bs || esc, !!(bs && esc));
  const key = j.title + '\u0001' + j.subtitle + '\u0001' + JSON.stringify(j.rows);
  const rows = uiBody.querySelectorAll('.ui-row');
  if (uiKind === 'list' && key === uiListKey && rows.length === j.rows.length) {
    // the same rows (a key moved the selection, or a refresh): only the highlight moves
    rows.forEach((r, i) => r.classList.toggle('sel', i === j.sel));
    if (rows[j.sel]) rows[j.sel].scrollIntoView({ block: 'nearest' });
    return;
  }
  const keepScroll = uiKind === 'list' && uiTitle === j.title ? uiBody.scrollTop : -1;
  uiKind = 'list'; uiListKey = key; uiTitle = j.title;
  uiBody.innerHTML = '';
  const w = el('div', 'ui-wrap');
  if (j.logo) {
    const hero = el('div', 'ui-hero');
    hero.append(el('h1', null, j.title));
    if (j.subtitle) hero.append(el('p', null, j.subtitle));
    w.append(hero);
  }
  j.rows.forEach((r, i) => {
    const b = el('div', 'ui-row' + (i === j.sel ? ' sel' : ''));
    b.setAttribute('role', 'button');
    if (r.p && r.pw && r.ph) b.append(pictureOf(r));
    else if (r.i && ICONS[r.i]) b.append(el('div', 'ui-ico', ICONS[r.i]));
    const tx = el('div', 'ui-txt');
    tx.append(el('b', null, r.l));
    if (r.s) tx.append(el('small', null, r.s));
    b.append(tx);
    if (r.m !== undefined && r.m >= 0) {
      const m = el('div', 'ui-meter');
      for (let k = 0; k < 10; k++) m.append(el('i', k < r.m ? 'on' : ''));
      b.append(m);
    }
    if (r.a) {
      // a value changed in place: ◀ ▶ are Left/Right on this row
      const a = el('div', 'ui-adj');
      const mk = (txt, code) => {
        const x = el('button', null, txt);
        x.addEventListener('click', (e) => { e.stopPropagation(); uiReply(code, i); });
        return x;
      };
      a.append(mk('◀', K.LEFT));
      if (r.v) a.append(el('span', 'ui-val', r.v));
      a.append(mk('▶', K.RIGHT));
      b.append(a);
    } else {
      if (r.v) b.append(el('span', 'ui-val', r.v));
      b.append(el('span', 'ui-chev', '›'));
    }
    b.addEventListener('click', () => uiReply(K.ENTER, i));
    w.append(b);
  });
  uiBody.append(w);
  if (keepScroll >= 0) uiBody.scrollTop = keepScroll;
  else { uiBody.scrollTop = 0; const s = w.querySelectorAll('.ui-row')[j.sel]; if (s && j.sel > 0) s.scrollIntoView({ block: 'center' }); }
}

function renderMsg(j) {
  uiKind = 'msg'; uiListKey = ''; uiTitle = null;
  setHead('', '', 0);
  uiBody.innerHTML = '';
  const c = el('div', 'ui-card');
  c.append(el('h2', null, j.title || ''));
  if (j.line) c.append(el('p', null, j.line));
  uiBody.append(c);
}

// A name typed with the phone's own keyboard; only the final text goes back, at OK.
function renderEdit(j) {
  uiKind = 'edit'; uiListKey = ''; uiTitle = null;
  setHead(j.title, '', K.ESC, t('ui.cancel'));
  uiBody.innerHTML = '';
  const c = el('div', 'ui-card');
  if (j.prompt) c.append(el('p', null, j.prompt));
  const inp = el('input');
  inp.type = 'text'; inp.value = j.text || ''; inp.maxLength = Math.max(1, j.cap || 63);
  inp.autocomplete = 'off'; inp.spellcheck = false; inp.setAttribute('autocapitalize', 'off');
  const act = el('div', 'ui-actions');
  const cancel = el('button', null, t('ui.cancel')), ok = el('button', 'primary', t('ui.ok'));
  const done = () => {
    const v = inp.value.trim();
    if (!v) { inp.focus(); return; }
    M.stringToUTF8(v, M._web_ui_textbuf(), 256);
    uiReply(K.ENTER);
  };
  ok.addEventListener('click', done);
  cancel.addEventListener('click', () => uiReply(K.ESC));
  inp.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); done(); }
    else if (e.key === 'Escape') { e.preventDefault(); uiReply(K.ESC); }
  });
  act.append(cancel, ok);
  c.append(inp, act);
  uiBody.append(c);
  setTimeout(() => { inp.focus(); inp.select(); }, 50);
}

// ======================= layout =======================
let kbdOn = true;
let padOn = false;                    // 🖱: the trackpad in the keyboard's place
let gameOn = false;                   // 🎮: the game keyboard in the PC keyboard's place
function layout() {
  const W = window.innerWidth, H = window.innerHeight;
  app.classList.remove('portrait', 'side', 'overlay', 'gsplit');
  // the screen's box: square for the panel, 4:3 for the PC's picture
  const ar = screenKind === 'pc' ? 4 / 3 : 1;
  app.style.setProperty('--ar', ar === 1 ? '1' : '4 / 3');
  if (H < W && gameOn && kbdOn && !padOn) {
    // landscape game keys: the screen in the middle, half the keys on each side of it
    app.classList.add('gsplit');
    const side = Math.max(150, (W - H * ar) / 2);
    app.style.setProperty('--scrw', Math.max(200, W - 2 * side) + 'px');
  } else if (H >= W) {
    app.classList.add('portrait');
    // the keyboard's rows share what the screen leaves (at least 40% of the height)
    const scr = Math.min(W / ar, H * (kbdOn || padOn ? 0.6 : 0.85));
    app.style.setProperty('--scrh', scr + 'px');
    app.style.setProperty('--kh', Math.max(30, Math.min(46, (H - scr - 100) / 6.4)) + 'px');
  } else if (W - H * ar >= 300) {
    app.classList.add('side');
    app.style.setProperty('--scrw', H * ar + 'px');
    app.style.setProperty('--kh', Math.max(26, Math.min(46, (H - 100) / 6.6)) + 'px');
  } else {
    app.classList.add('overlay');
    app.style.setProperty('--kh', Math.max(24, Math.min(36, H / 13)) + 'px');
  }
  // narrow landscape lays everything over the screen, which is already a touchpad: no pad there
  const pad = padOn && !app.classList.contains('overlay');
  app.classList.toggle('padmode', pad);
  $('#pad').hidden = !pad;
  kbd.hidden = !kbdOn || padOn;
  if (kbd.classList.contains('game') !== gameOn) buildKbd();
  $('[data-act=kbd]').classList.toggle('on', kbdOn && !padOn && !gameOn);
  $('[data-act=game]').classList.toggle('on', kbdOn && gameOn && !padOn);
  $('[data-act=mouse]').classList.toggle('on', padOn);
  requestAnimationFrame(fitPc);
}
window.addEventListener('resize', layout);
window.addEventListener('orientationchange', () => setTimeout(layout, 200));

// ======================= keys =======================
function key(code, down) { if (M) M._web_key(code, down ? 1 : 0); }
function tapKey(code, mods = []) {
  mods.forEach((m) => key(m, true));
  key(code, true);
  setTimeout(() => { key(code, false); mods.forEach((m) => key(m, false)); }, 80);
}

// The physical keyboard, as the desktop build sends it (hal_sdl.cpp): special keys by name, the rest
// as the character typed. Letters go lower case unless Shift is held, because pc_keymap keeps its
// own Caps Lock (Caps Lock itself is sent). pc_keymap repeats held keys, so the browser's repeats
// are dropped; a release goes by e.code to whatever its press sent.
const SPECIAL = {
  Enter: K.ENTER, Backspace: K.BS, Tab: K.TAB, Escape: K.ESC,
  ArrowLeft: K.LEFT, ArrowUp: K.UP, ArrowDown: K.DOWN, ArrowRight: K.RIGHT,
  Control: K.CTRL, Alt: K.ALT, AltGraph: K.ALT, CapsLock: K.CAPS, Insert: K.INS, Home: K.HOME, Delete: K.DEL,
  End: K.END, PageUp: K.PGUP, PageDown: K.PGDN, Pause: K.BREAK,
  F1: K.F[1], F2: K.F[2], F3: K.F[3], F4: K.F[4], F5: K.F[5], F6: K.F[6], F7: K.F[7], F8: K.F[8], F9: K.F[9], F10: K.F[10],
};
const dialogOpen = () => !$('#files').hidden || !$('#boot').hidden ||
  (document.activeElement && document.activeElement.tagName === 'INPUT');
const downByCode = new Map();          // e.code -> the code its press sent
function codeOf(e) {
  if (e.key === 'Shift') return e.code === 'ShiftRight' ? K.SHIFT_R : K.SHIFT_L;
  if (SPECIAL[e.key] !== undefined) return SPECIAL[e.key];
  if (e.key && e.key.length === 1) {
    let c = e.key;
    if (/[a-z]/i.test(c)) c = e.shiftKey ? c.toUpperCase() : c.toLowerCase();
    const n = c.charCodeAt(0);
    if (n >= 32 && n < 127) return n;
  }
  return 0;
}
window.addEventListener('keydown', (e) => {
  if (dialogOpen() || e.metaKey) return;
  const c = codeOf(e);
  if (!c) return;
  e.preventDefault();
  if (e.repeat || downByCode.has(e.code)) return;
  downByCode.set(e.code, c);
  key(c, true);
});
window.addEventListener('keyup', (e) => {
  const c = downByCode.get(e.code);
  if (c === undefined) return;
  e.preventDefault();
  downByCode.delete(e.code);
  key(c, false);
});
function releaseAll() {
  for (const c of downByCode.values()) key(c, false);
  downByCode.clear();
  releaseLatched();
  setButtons(0);
}
window.addEventListener('blur', releaseAll);

// ======================= on-screen keyboard =======================
// Kinds of key: a character (c, with sc the one Shift gives), a fixed code, or a modifier that
// latches until the next key (Ctrl, Alt, Shift). A key is held while the finger stays on it, so
// pc_keymap's typematic repeat works as on the device.
const CH = (c, sc, w = 1) => ({ l: c, c, sc, top: sc, w });
const KC = (l, code, w = 1, cls = 'mod') => ({ l, code, w, cls });
const MOD = (l, code, w = 1) => ({ l, code, w, cls: 'mod', latch: true });
const letters = (s) => s.split('').map((c) => ({ l: c.toUpperCase(), c, w: 1 }));
const ROWS = [
  [KC('Esc', K.ESC, 1.2), ...[1, 2, 3, 4, 5, 6, 7, 8, 9, 10].map((n) => KC('F' + n, K.F[n], 1, 'fkey')), KC('Ins', K.INS), KC('Del', K.DEL)],
  [CH('`', '~'), ...'1234567890'.split('').map((c, i) => CH(c, '!@#$%^&*()'[i])), CH('-', '_'), CH('=', '+'), KC('⌫', K.BS, 1.5)],
  [KC('Tab', K.TAB, 1.4), ...letters('qwertyuiop'), CH('[', '{'), CH(']', '}'), CH('\\', '|')],
  [KC('Caps', K.CAPS, 1.6), ...letters('asdfghjkl'), CH(';', ':'), CH("'", '"'), KC('↵', K.ENTER, 1.8)],
  [MOD('Shift', K.SHIFT_L, 2), ...letters('zxcvbnm'), CH(',', '<'), CH('.', '>'), CH('/', '?'), KC('PgUp', K.PGUP), KC('↑', K.UP), KC('PgDn', K.PGDN)],
  [MOD('Ctrl', K.CTRL, 1.4), MOD('Alt', K.ALT, 1.2), { l: '', code: 0x20, w: 5 }, KC('Home', K.HOME), KC('End', K.END), KC('←', K.LEFT), KC('↓', K.DOWN), KC('→', K.RIGHT)],
];

// The game keyboard (🎮): the keys games use, big, about where they sit on a real keyboard (WASD at
// the left, the arrows at the right), on a 14-column grid: x, y and the width in columns. Its Ctrl,
// Shift and Alt are held while the finger stays on them (fire, run), not latched. The arrows are a
// 3x3 pad whose corners hold two arrows at once (codes), for diagonal moves; its middle is a
// second Down, so all four arrows sit in an inverted T as on a keyboard. A finger that slides from
// one arrow to another changes the direction (pad keys), as on a game pad's cross.
// In landscape (gsplit) the keys split to both sides of the screen, for two thumbs: S() gives each
// its place there, left (WASD side, 8 columns) or right (arrows, 6 columns). Keys marked 'split'
// show only then: a second Shift and Space by the arrows.
const GK = (l, code, x, y, w = 2, cls) => ({ l, code, x, y, w, cls });
const GC = (c, x, y) => ({ l: c.toUpperCase(), c, x, y, w: 2 });
const GD = (l, a, b, x, y) => ({ l, codes: [a, b], x, y, w: 2, cls: 'diag', pad: true });
const GA = (l, code, x, y) => ({ l, codes: [code], x, y, w: 2, pad: true });
const S = (k, side, x, y, w = 2) => Object.assign(k, { side, sx: x, sy: y, sw: w });
const GAME = [
  S(GK('Esc', K.ESC, 1, 1, 2, 'mod'), 'l', 1, 1), S(GC('w', 5, 1), 'l', 5, 1),
  S(GK('⌫', K.BS, 9, 1, 2, 'mod'), 'r', 1, 1), S(GK('Enter', K.ENTER, 11, 1, 4, 'mod'), 'r', 3, 1, 4),
  S(GK('Tab', K.TAB, 1, 2, 2, 'mod'), 'l', 1, 2), S(GC('a', 3, 2), 'l', 3, 2), S(GC('s', 5, 2), 'l', 5, 2), S(GC('d', 7, 2), 'l', 7, 2),
  S(GD('↖', K.UP, K.LEFT, 9, 2), 'r', 1, 2), S(GA('↑', K.UP, 11, 2), 'r', 3, 2), S(GD('↗', K.UP, K.RIGHT, 13, 2), 'r', 5, 2),
  S(GK('Shift', K.SHIFT_L, 1, 3, 4, 'mod'), 'l', 1, 3, 4),
  S(GA('←', K.LEFT, 9, 3), 'r', 1, 3), S(GA('↓', K.DOWN, 11, 3), 'r', 3, 3), S(GA('→', K.RIGHT, 13, 3), 'r', 5, 3),
  S(GK('Ctrl', K.CTRL, 1, 4, 2, 'mod'), 'l', 1, 4), S(GK('Alt', K.ALT, 3, 4, 2, 'mod'), 'l', 3, 4), S(GK('Space', 0x20, 5, 4, 4), 'l', 5, 4, 4),
  S(GD('↙', K.DOWN, K.LEFT, 9, 4), 'r', 1, 4), S(GA('↓', K.DOWN, 11, 4), 'r', 3, 4), S(GD('↘', K.DOWN, K.RIGHT, 13, 4), 'r', 5, 4),
  S(GK('Shift', K.SHIFT_R, 0, 0, 2, 'mod split'), 'r', 1, 5, 3), S(GK('Space', 0x20, 0, 0, 2, 'split'), 'r', 4, 5, 3),
];

const kbd = $('#kbd');
const latched = new Set();
function setLatch(code, on) {
  if (on) latched.add(code); else latched.delete(code);
  key(code, on);
  kbd.querySelectorAll(`[data-latch="${code}"]`).forEach((x) => x.classList.toggle('latch', on));
}
function releaseLatched() { for (const c of [...latched]) setLatch(c, false); }

function oskCode(k) {
  if (k.c === undefined) return k.code;
  const sh = latched.has(K.SHIFT_L);
  let c = sh && k.sc ? k.sc : k.c;
  if (sh && /[a-z]/.test(c)) c = c.toUpperCase();
  return c.charCodeAt(0);
}

function makeKey(k) {
  const b = document.createElement('button');
  b.className = 'k' + (k.cls ? ' ' + k.cls : '');
  if (k.top) { const s = document.createElement('small'); s.textContent = k.top; b.appendChild(s); }
  const m = document.createElement('span'); m.textContent = k.l; b.appendChild(m);
  if (k.latch) {
    b.dataset.latch = k.code;
    b.addEventListener('pointerdown', (e) => { e.preventDefault(); setLatch(k.code, !latched.has(k.code)); });
  } else {
    let downAt = 0, sent = null;      // the codes the press sent (two for a diagonal arrow)
    let cur = b;                      // the key the finger is on: another pad key after a slide
    b.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      try { b.setPointerCapture(e.pointerId); } catch (_) {}
      if (sent) return;
      cur = b;
      b.classList.add('down');
      downAt = performance.now();
      sent = k.codes || [oskCode(k)];
      sent.forEach((c) => key(c, true));
    });
    if (k.pad) {
      b._codes = k.codes;
      // the capture keeps the moves here: look for the pad key under the finger (off the pad, the
      // last one stays held)
      b.addEventListener('pointermove', (e) => {
        if (!sent) return;
        const t = document.elementFromPoint(e.clientX, e.clientY);
        const n = t && t.closest('#kbd .k');
        if (!n || n === cur || !n._codes) return;
        const next = n._codes;
        sent.filter((c) => !next.includes(c)).forEach((c) => key(c, false));
        next.filter((c) => !sent.includes(c)).forEach((c) => key(c, true));
        sent = next;
        cur.classList.remove('down'); n.classList.add('down'); cur = n;
      });
    }
    const up = () => {
      if (!sent) return;
      const cs = sent; sent = null;
      cur.classList.remove('down');
      // let the guest see the press before the release (and the latches after both)
      const wait = Math.max(0, 70 - (performance.now() - downAt));
      setTimeout(() => { cs.forEach((c) => key(c, false)); if (latched.size) setTimeout(releaseLatched, 30); }, wait);
    };
    b.addEventListener('pointerup', up); b.addEventListener('pointercancel', up);
  }
  b.addEventListener('contextmenu', (e) => e.preventDefault());
  return b;
}

function buildKbd() {
  releaseLatched();
  kbd.innerHTML = '';
  kbd.classList.toggle('game', gameOn);
  if (gameOn) {
    // both places are set; the CSS picks one (the portrait grid, or gsplit's two side grids)
    const half = { l: document.createElement('div'), r: document.createElement('div') };
    half.l.className = 'gl'; half.r.className = 'gr';
    for (const k of GAME) {
      const b = makeKey(k);
      b.style.setProperty('--pc', `${k.x} / span ${k.w}`);
      b.style.setProperty('--pr', String(k.y));
      b.style.setProperty('--sc', `${k.sx} / span ${k.sw}`);
      b.style.setProperty('--sr', String(k.sy));
      half[k.side].appendChild(b);
    }
    kbd.append(half.l, half.r);
    return;
  }
  for (const row of ROWS) {
    const r = document.createElement('div'); r.className = 'krow';
    for (const k of row) {
      const b = makeKey(k);
      b.style.flexGrow = k.w;
      r.appendChild(b);
    }
    kbd.appendChild(r);
  }
}
buildKbd();
layout();

// ======================= mouse =======================
// The canvas, and the trackpad 🖱 puts in the keyboard's place, are touchpads: a finger's moves are
// sent relative (web_mouse), a quick tap is a left click and a two-finger tap a right click; the
// Left/Right buttons below hold for drags, and a pinch on the screen zooms. With a real
// mouse, a click on the screen locks the pointer and its moves go straight to the guest (Esc
// unlocks, as the browser wants). A finger's move counts in the PC picture's own pixels (at least
// 640 across, so a 320x200 game's pointer is not twice as fast).
let btnHeld = 0, btnTap = 0;
function sendMouse(dx, dy) { if (M) M._web_mouse(dx, dy, btnHeld | btnTap); }
function setButtons(b) { btnHeld = b; sendMouse(0, 0); document.querySelectorAll('.mb').forEach((x) => x.classList.toggle('on', !!(b & +x.dataset.mb))); }
function click(bit) {
  btnTap |= bit; sendMouse(0, 0);
  setTimeout(() => { btnTap &= ~bit; sendMouse(0, 0); }, 90);
}

let fracX = 0, fracY = 0;
// A finger's move in CSS pixels -> guest pixels: across the screen's width is across the picture.
function touchMove(dxCss, dyCss) {
  const w = pcCanvas.getBoundingClientRect().width || wrap.getBoundingClientRect().width || 320;
  // guest pixels, not the canvas's device pixels; zoomed in, a finger's move covers less of the picture
  const k = Math.max(640, pcImg ? pcImg.width : 640) / w / zoom;
  fracX += dxCss * k; fracY += dyCss * k;
  const ix = Math.trunc(fracX), iy = Math.trunc(fracY);
  if (ix || iy) { fracX -= ix; fracY -= iy; sendMouse(ix, iy); }
}
// Touch gestures on el (the screen, and the trackpad 🖱 shows in the keyboard's place): a finger
// moves the pointer, a tap is a left click, a two-finger tap a right click, and a tap followed at
// once by a touch that moves drags with the left button held (as laptop trackpads do).
// mouseToo: a real mouse dragging on el acts as a finger (the trackpad; the screen locks instead).
// zoomable: two fingers that spread, close or move together zoom and pan the PC's picture instead
// (back to the whole picture below 1.03x); they move no pointer and click nothing.
function bindTouchpad(el, mouseToo, zoomable) {
  const touches = new Map();          // pointerId -> { x, y, t, moved }
  let multi = false, lastTap = 0, drag = false;
  let pinch = null, gesture = false;  // two fingers down / they zoomed or panned since the first went down
  const twoOf = () => {
    const [p, q] = [...touches.values()], r = pcCanvas.getBoundingClientRect();
    return { d: Math.hypot(p.x - q.x, p.y - q.y), mx: ((p.x + q.x) / 2 - r.left) / r.width,
      my: ((p.y + q.y) / 2 - r.top) / r.height, r };
  };
  el.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    if (!M || uiMode !== 'pc' || !M._web_pc_running()) return;
    if (e.pointerType === 'mouse' && !mouseToo) {
      if (document.pointerLockElement !== wrap) { wrap.requestPointerLock && wrap.requestPointerLock(); return; }
      return;                         // locked: the window's mouse events below take it
    }
    try { el.setPointerCapture(e.pointerId); } catch (_) {}
    touches.set(e.pointerId, { x: e.clientX, y: e.clientY, t: performance.now(), moved: 0 });
    if (touches.size > 1) {
      multi = true;
      if (zoomable && touches.size === 2 && screenKind === 'pc') {
        const g = twoOf();
        pinch = { d0: g.d, mx0: g.mx, my0: g.my, s0: zoom, a: zu + g.mx / zoom, b: zv + g.my / zoom };
      }
    } else if (performance.now() - lastTap < 300) { drag = true; setButtons(btnHeld | 1); }
    el.classList.add('touch');
  });
  el.addEventListener('pointermove', (e) => {
    const p = touches.get(e.pointerId);
    if (!p) return;
    const dx = e.clientX - p.x, dy = e.clientY - p.y;
    p.x = e.clientX; p.y = e.clientY; p.moved += Math.abs(dx) + Math.abs(dy);
    if (pinch && touches.size === 2) {
      const g = twoOf();
      if (!gesture && Math.abs(g.d - pinch.d0) < 14 &&
          Math.hypot((g.mx - pinch.mx0) * g.r.width, (g.my - pinch.my0) * g.r.height) < 14) return;
      gesture = true;
      zoom = Math.min(8, Math.max(1, pinch.s0 * g.d / Math.max(1, pinch.d0)));
      if (zoom < 1.03) zoom = 1;
      // the picture's point that was under the fingers' midpoint stays under it
      zu = Math.min(1 - 1 / zoom, Math.max(0, pinch.a - g.mx / zoom));
      zv = Math.min(1 - 1 / zoom, Math.max(0, pinch.b - g.my / zoom));
      drawPc();
      return;
    }
    if (gesture) return;              // a finger left over from a pinch moves nothing
    // two fingers: only the first one steers
    if (touches.keys().next().value === e.pointerId) touchMove(dx, dy);
  });
  for (const ev of ['pointerup', 'pointercancel']) el.addEventListener(ev, (e) => {
    const p = touches.get(e.pointerId);
    if (!p) return;
    touches.delete(e.pointerId);
    pinch = null;
    const tap = ev === 'pointerup' && performance.now() - p.t < 250 && p.moved < 10;
    if (touches.size === 0) {
      el.classList.remove('touch');
      if (gesture) {                  // a pinch: no click
        gesture = false; lastTap = 0;
        if (drag) { drag = false; setButtons(btnHeld & ~1); }
      } else if (drag) {                     // the end of a drag; a quick second tap is a double click
        drag = false; setButtons(btnHeld & ~1);
        if (tap) click(1);
        lastTap = 0;
      } else if (tap) {
        click(multi ? 2 : 1);
        lastTap = multi ? 0 : performance.now();
      } else lastTap = 0;
      multi = false;
    }
  });
  el.addEventListener('contextmenu', (e) => e.preventDefault());
}
bindTouchpad(wrap, false, true);
bindTouchpad($('#pad'), true, false);

document.addEventListener('pointerlockchange', () => wrap.classList.toggle('locked', document.pointerLockElement === wrap));
document.addEventListener('mousemove', (e) => {
  if (document.pointerLockElement === wrap) sendMouse(e.movementX, e.movementY);
});
document.addEventListener('mousedown', (e) => {
  if (document.pointerLockElement !== wrap) return;
  const bit = e.button === 0 ? 1 : e.button === 2 ? 2 : 0;
  if (bit) setButtons(btnHeld | bit);
});
document.addEventListener('mouseup', (e) => {
  if (document.pointerLockElement !== wrap) return;
  const bit = e.button === 0 ? 1 : e.button === 2 ? 2 : 0;
  if (bit) setButtons(btnHeld & ~bit);
});

document.querySelectorAll('.mb').forEach((b) => {
  const bit = +b.dataset.mb;
  b.addEventListener('pointerdown', (e) => { e.preventDefault(); try { b.setPointerCapture(e.pointerId); } catch (_) {} setButtons(btnHeld | bit); });
  for (const ev of ['pointerup', 'pointercancel']) b.addEventListener(ev, () => setButtons(btnHeld & ~bit));
  b.addEventListener('contextmenu', (e) => e.preventDefault());
});

// (the touchpad's buttons show only while a PC runs: setMode)

// ======================= toolbar =======================
$('#toolbar').addEventListener('click', (e) => {
  const b = e.target.closest('button'); if (!b) return;
  startAudio();
  switch (b.dataset.act) {
    case 'menu': tapKey(K.ESC, [K.CTRL]); break;             // the in-game menu (a no-op in the home menu)
    case 'kbd': if (padOn || gameOn) { padOn = gameOn = false; kbdOn = true; } else kbdOn = !kbdOn; layout(); break;
    case 'game': gameOn = !(gameOn && kbdOn && !padOn); padOn = false; kbdOn = true; layout(); break;
    case 'files': openFiles(); break;
    case 'mouse': padOn = !padOn; if (padOn) gameOn = false; else kbdOn = true; layout(); break;   // the trackpad instead of the keyboard
    case 'sound': toggleMute(); break;
    case 'fs': toggleFullscreen(); break;
  }
});
function toggleFullscreen() {
  const d = document;
  if (!d.fullscreenElement && !d.webkitFullscreenElement) {
    const el = d.documentElement;
    (el.requestFullscreen || el.webkitRequestFullscreen || (() => {})).call(el, { navigationUI: 'hide' });
  } else {
    (d.exitFullscreen || d.webkitExitFullscreen).call(d);
  }
}

// ======================= audio =======================
let actx = null, gain = null, muted = false;
function startAudio() {
  // iOS: play through the ring/silent switch like a media app (Safari 16.4+)
  try { if (navigator.audioSession) navigator.audioSession.type = 'playback'; } catch (e) {}
  if (actx) { if (actx.state !== 'running') actx.resume(); return; }
  if (!M) return;
  try {
    actx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
    actx.resume();                                          // still inside the user gesture
    // a silent one-sample buffer, started in the gesture, unlocks older iOS WebKit
    const sb = actx.createBuffer(1, 1, 22050), src = actx.createBufferSource();
    src.buffer = sb; src.connect(actx.destination); src.start(0);
  } catch (e) { console.warn('audio', e); return; }
  actx.audioWorklet.addModule('audio-worklet.js').then(() => {
    const node = new AudioWorkletNode(actx, 'pico386-ring', { numberOfInputs: 0, outputChannelCount: [1] });
    node.port.postMessage({ buf: M.HEAPU8.buffer, ptr: M._web_audio_ring() });
    gain = actx.createGain(); gain.gain.value = muted ? 0 : 1;
    node.connect(gain).connect(actx.destination);
  }).catch((e) => console.warn('audio worklet', e));
}
function toggleMute() {
  muted = !muted;
  if (gain) gain.gain.value = muted ? 0 : 1;
  $('[data-act=sound]').textContent = muted ? '🔇' : '🔊';
}
for (const ev of ['pointerdown', 'touchend', 'keydown']) document.addEventListener(ev, () => { if (M) startAudio(); }, true);

// ======================= files =======================
const folderSel = $('#folder');
FOLDERS.forEach((f) => { const o = document.createElement('option'); o.value = o.textContent = f; folderSel.appendChild(o); });
folderSel.value = 'pico-386/disks';
folderSel.addEventListener('change', refreshFiles);

function listDir(dir) {
  try {
    return M.FS.readdir(dir).filter((n) => n !== '.' && n !== '..').map((n) => {
      const st = M.FS.stat(dir + '/' + n);
      return { name: n, size: st.size, dir: M.FS.isDir(st.mode) };
    });
  } catch (e) { return []; }
}
function fmtSize(n) { return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : n >= 1024 ? Math.round(n / 1024) + ' KB' : n + ' B'; }
function refreshFiles() {
  const ul = $('#file-list'); ul.innerHTML = '';
  if (!M) return;
  const dir = '/sdcard/' + folderSel.value;
  // the folders have their own entry in the select; the dot folders (.unzipped, temporaries) are the emulator's
  const items = listDir(dir).filter((it) => !it.dir && !it.name.startsWith('.')).sort((a, b) => a.name.localeCompare(b.name));
  if (!items.length) { const li = document.createElement('li'); li.innerHTML = '<span class="hint"></span>'; li.firstChild.textContent = t('files.empty'); ul.appendChild(li); }
  for (const it of items) {
    const li = document.createElement('li');
    const s = document.createElement('span'); s.textContent = it.name;
    const sm = document.createElement('small'); sm.textContent = fmtSize(it.size); s.appendChild(sm);
    // a disk the guest wrote to can be taken home again
    const dl = document.createElement('button'); dl.textContent = t('files.download');
    dl.onclick = () => {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([M.FS.readFile(dir + '/' + it.name)]));
      a.download = it.name; a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    };
    const del = document.createElement('button'); del.textContent = t('files.delete');
    del.onclick = () => {
      if (!confirm(t('files.confirmDelete', it.name))) return;
      try { M.FS.unlink(dir + '/' + it.name); } catch (e) { alert(t('files.deleteFail', e)); }
      refreshFiles(); schedulePersist();
    };
    li.append(s, dl, del); ul.appendChild(li);
  }
}
function openFiles() { releaseAll(); $('#files').hidden = false; refreshFiles(); }
$('#files [data-close]').addEventListener('click', () => { $('#files').hidden = true; recvStop(); });
$('#upload').addEventListener('change', async (e) => {
  const files = [...e.target.files]; e.target.value = '';
  if (!M || !files.length) return;
  const st = $('#upload-status');
  const dir = '/sdcard/' + folderSel.value;
  let n = 0;
  for (const f of files) {
    st.textContent = t('files.uploading', f.name);
    try { M.FS.writeFile(dir + '/' + f.name, new Uint8Array(await f.arrayBuffer())); n++; }
    catch (err) { alert(t('files.uploadFail', f.name, err)); }
  }
  persist(() => { st.textContent = t('files.saved', n, folderSel.value); });
  refreshFiles();
});

// ---- receive from PC: the phone opens a relay room (worker/relay.js) under a short code; the
// desktop page /send joins it and streams files: {"t":"file",name,size,folder} + binary chunks +
// {"t":"end"}. Each file is written to /sdcard/<folder> and answered with {"t":"saved",path}.
const EXT_FOLDER = { img: 'pico-386/disks', ima: 'pico-386/disks', vfd: 'pico-386/disks', iso: 'pico-386/disks',
  zip: 'pico-386/games', bin: 'pico-386/roms', ini: 'pico-386' };
const RECV_IDLE_MS = 10 * 60 * 1000, RECV_MAX = 128 * 1024 * 1024;
let recv = null;   // { ws, code, idle, cur:{name,size,folder,buf,got} }

function recvCode() {
  const A = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789', r = crypto.getRandomValues(new Uint8Array(5));
  return [...r].map((b) => A[b % A.length]).join('');
}
function recvStatus(k, ...a) { $('#recv-status').textContent = t(k, ...a); }
function recvTouch() { clearTimeout(recv.idle); recv.idle = setTimeout(recvStop, RECV_IDLE_MS); }
function recvFolder(name, asked) {
  if (FOLDERS.includes(asked)) return asked;
  const ext = (name.match(/\.([^.]+)$/) || [])[1];
  return EXT_FOLDER[(ext || '').toLowerCase()] || folderSel.value;
}

function recvStart() {
  recvStop();
  const code = recvCode();
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${proto}//${location.host}/api/relay/${code}?role=recv`);
  ws.binaryType = 'arraybuffer';
  recv = { ws, code, idle: 0, cur: null };
  $('#recv').hidden = false; $('#btn-recv').hidden = true;
  $('#recv-code').textContent = code;
  $('#recv-url').textContent = location.origin + '/send?c=' + code;
  recvStatus('recv.connecting');
  recvTouch();
  const reply = (o) => { try { ws.send(JSON.stringify(o)); } catch (e) {} };
  ws.onopen = () => recvStatus('recv.waiting');
  ws.onclose = () => { if (recv && recv.ws === ws) { recvStatus('recv.closed'); recv.ws = null; } };
  ws.onmessage = (e) => {
    if (!recv || recv.ws !== ws) return;
    recvTouch();
    const c = recv.cur;
    if (typeof e.data !== 'string') {           // a chunk of the current file
      if (!c || c.bad) return;
      const b = new Uint8Array(e.data);
      if (c.got + b.length > c.size) { c.bad = 'size'; return; }
      c.buf.set(b, c.got); c.got += b.length;
      recvStatus('recv.getting', c.name, Math.floor(100 * c.got / Math.max(1, c.size)));
      return;
    }
    let m; try { m = JSON.parse(e.data); } catch (err) { return; }
    if (m.t === 'peer') recvStatus(m.on ? 'recv.pc' : 'recv.waiting');
    else if (m.t === 'file') {
      const name = String(m.name || '').replace(/[\/:*?"<>|\x00-\x1f]/g, '_').replace(/^\.+/, '').slice(0, 120);
      const size = Number(m.size) | 0;
      if (!name || size < 0 || size > RECV_MAX) { recv.cur = { name: name || '?', bad: 'size' }; return; }
      recv.cur = { name, size, folder: recvFolder(name, m.folder), buf: new Uint8Array(size), got: 0 };
      recvStatus('recv.getting', name, 0);
    } else if (m.t === 'end' && c) {
      recv.cur = null;
      if (c.bad || c.got !== c.size) { reply({ t: 'error', name: c.name, msg: t('recv.badFile') }); return; }
      const dir = '/sdcard/' + c.folder;
      try {
        mkdirs(M.FS, dir);
        M.FS.writeFile(dir + '/' + c.name, c.buf);
      } catch (err) { reply({ t: 'error', name: c.name, msg: String(err) }); return; }
      reply({ t: 'saved', name: c.name, path: c.folder + '/' + c.name });
      recvStatus('recv.saved', c.name, c.folder);
      refreshFiles(); schedulePersist();
    }
  };
}
function recvStop() {
  if (!recv) return;
  clearTimeout(recv.idle);
  if (recv.ws) { recv.ws.onclose = null; try { recv.ws.close(); } catch (e) {} }
  recv = null;
  $('#recv').hidden = true; $('#btn-recv').hidden = false;
}
$('#btn-recv').addEventListener('click', () => { if (M) recvStart(); });
$('#btn-recv-stop').addEventListener('click', recvStop);
$('#btn-reboot').addEventListener('click', () => { if (M && confirm(t('confirm.reboot'))) M.webReboot(); });

// language picker (Files dialog): re-label everything built in JS, too
const langSel = $('#lang');
for (const [k, d] of Object.entries(I18N)) { const o = document.createElement('option'); o.value = k; o.textContent = d._name; langSel.appendChild(o); }
langSel.value = LANG;
langSel.addEventListener('change', () => setLang(langSel.value));
document.addEventListener('pico386-lang', () => {
  if (!$('#files').hidden) refreshFiles();
  $('#upload-status').textContent = '';
});
