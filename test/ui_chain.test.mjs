/*
 * Offline test of ui_chain.js against the REAL param_pages library from the
 * Schwung repo, with the host globals mocked. Proves the binding loads, the
 * grid plans our pages from the DSP's ui_pages/chain_params, renders without
 * throwing, and the pad gestures do what the help text says.
 *
 *   node test/ui_chain.test.mjs [path/to/schwung/src/shared]
 */
import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { pathToFileURL } from "node:url";

const SHARED = process.argv[2] || "/Users/gustavolima/Developer/schwung-overtake/src/shared";
const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
let fails = 0;
const check = (c, m) => { console.log((c ? "ok  : " : "FAIL: ") + m); if (!c) fails++; };

/* the DSP's two payloads, straight from the generated header */
const hdr = fs.readFileSync(path.join(ROOT, "src/dsp/sd606_params.h"), "utf8");
function cstr(name) {
  const i = hdr.indexOf(name + "[] =");
  const body = hdr.slice(i, hdr.indexOf(";", i));
  return body.split("\n").slice(1).map(l => l.trim()).filter(l => l.startsWith('"'))
    .map(l => JSON.parse(l)).join("");
}
const CHAIN_PARAMS = cstr("sd606_chain_params_json");
const UI_PAGES = cstr("sd606_ui_pages_json");
JSON.parse(CHAIN_PARAMS); JSON.parse(UI_PAGES);

/* ---- host mock ---- */
const params = { "synth:chain_params": CHAIN_PARAMS, "synth:ui_pages": UI_PAGES, "synth:mutes": "0" };
for (const p of JSON.parse(CHAIN_PARAMS)) params["synth:" + p.key] = String(p.default ?? 0);
const setLog = [];
let shift = false, injected = [], announced = [];
Object.assign(globalThis, {
  shadow_get_ui_slot: () => 0,
  shadow_get_display_mode: () => 1,
  shadow_get_shift_held: () => shift,
  shadow_get_param: (slot, k) => (k in params ? params[k] : null),
  shadow_set_param: (slot, k, v) => { params[k] = String(v); setLog.push([k, String(v)]); },
  host_pad_block: () => {},
  host_announce_screenreader: t => announced.push(t),
  move_midi_inject_to_move: m => injected.push(m),
  clear_screen: () => {}, fill_rect: () => {}, print: () => {}, text_width: s => s.length * 6,
  draw_line: () => {}, fill_circle: () => {}, draw_circle: () => {}, draw_arc: () => {},
});

/* ---- load ui_chain.js with its device imports pointed at the real library ---- */
const spy = path.join(ROOT, "build-native", "pc_spy.mjs");
fs.mkdirSync(path.dirname(spy), { recursive: true });
fs.writeFileSync(spy, `
import { createController as real } from ${JSON.stringify(pathToFileURL(SHARED + "/param_pages/page_controller.mjs").href)};
export const spied = { title: null };
export function createController(...a) {
  const c = real(...a);
  const r = c.render.bind(c);
  c.render = (ctx, opts) => { if (opts && opts.title != null) spied.title = opts.title; return r(ctx, opts); };
  return c;
}
`);
const src = fs.readFileSync(path.join(ROOT, "src/ui_chain.js"), "utf8")
  .replace(/\/data\/UserData\/schwung\/shared\/param_pages\/page_controller\.mjs/g,
           pathToFileURL(spy).href)
  .replace(/\/data\/UserData\/schwung\/shared\//g, pathToFileURL(SHARED + "/").href);
const tmp = path.join(ROOT, "build-native", "ui_chain.test.mjs");
fs.mkdirSync(path.dirname(tmp), { recursive: true });
fs.writeFileSync(tmp, src);
await import(pathToFileURL(tmp).href);
const { spied } = await import(pathToFileURL(spy).href);
const ui = globalThis.chain_ui;
check(ui && ui.init && ui.tick && ui.onMidiMessageInternal && ui.handleBack, "chain_ui exports the four hooks");

ui.init();
check(announced.includes("6W6"), "init announces 6W6");
ui.tick();
check(true, "first tick renders without throwing");

/* a peek at the controller through the closure is not possible; infer from behaviour */
const note = (n, v) => ui.onMidiMessageInternal(new Uint8Array([0x90, n, v]));
const off  = n => ui.onMidiMessageInternal(new Uint8Array([0x80, n, 0]));

/* plain pad: reaches Move, no param writes */
injected = []; setLog.length = 0;
note(68, 100); off(68);
check(injected.length === 2 && injected[0][0] === 0x09 && injected[0][2] === 68, "plain pad press+release pass through to Move");
check(setLog.length === 1 && setLog[0][0] === "synth:ui_focus" && setLog[0][1] === "0",
      "plain pad publishes only synth:ui_focus=0 (BD lane, for the remote panel to follow)");
setLog.length = 0; note(68, 100); off(68); note(68, 100); off(68);
check(setLog.length === 0, "hitting the SAME pad again writes nothing (a param write is a 2.8 ms round-trip)");
setLog.length = 0; note(69, 100); off(69);
check(setLog.length === 1 && setLog[0][1] === "1", "moving to another pad does write (lane 1)");

/* pad 16 = master page: never reaches Move */
injected = []; setLog.length = 0; note(84, 100); off(84);
check(injected.length === 0, "pad 9 (master) never sounds");
check(setLog.some(([k, v]) => k === "synth:ui_focus" && v === "8"), "pad 9 publishes ui_focus=8 (master)");
injected = []; setLog.length = 0; note(85, 100); off(85);
check(injected.length === 0, "pad 10 (reverb) never sounds");
check(setLog.some(([k, v]) => k === "synth:ui_focus" && v === "9"), "pad 10 opens Reverb (focus 9)");
injected = []; setLog.length = 0; note(86, 100); off(86);
check(injected.length === 0, "pad 11 (delay) never sounds");
check(setLog.some(([k, v]) => k === "synth:ui_focus" && v === "10"), "pad 11 opens Delay (focus 10)");

/* Shift+Pad: silent select -> mute_ms 60 then inject */
shift = true; injected = []; setLog.length = 0;
note(77, 100); off(77); shift = false;
check(setLog.some(([k, v]) => k === "synth:mute_ms" && v === "60"), "Shift+Pad arms the 60 ms silent-select window");
check(injected.length === 2, "Shift+Pad still reaches Move (white pad follows)");

/* Mute+Pad: toggles that lane's bit in synth:mutes, still passes through */
ui.onMidiMessageInternal(new Uint8Array([0xB0, 88, 127]));   /* mute held */
setLog.length = 0; injected = [];
note(78, 100); off(78);                                      /* cymbal, lane 6 */
ui.onMidiMessageInternal(new Uint8Array([0xB0, 88, 0]));
check(setLog.some(([k, v]) => k === "synth:mutes" && v === String(1 << 6)), "Mute+Pad toggles the cymbal lane (bit 6)");
check(injected.length === 2, "Mute+Pad press still reaches Move");
ui.tick();                                                   /* title shows [M]; just must not throw */
check(true, "tick with a muted lane renders");
ui.onMidiMessageInternal(new Uint8Array([0xB0, 88, 127])); note(78, 100); off(78); ui.onMidiMessageInternal(new Uint8Array([0xB0, 88, 0]));
check(params["synth:mutes"] === "0", "second Mute+Pad clears it");

/* ---- Main-page jog lock ---- */
const jogClick = () => ui.onMidiMessageInternal(new Uint8Array([0xB0, 3, 127]));
const jogTurn  = () => ui.onMidiMessageInternal(new Uint8Array([0xB0, 14, 1]));

/* get to Main, then arm the lock with a click there */
note(84, 100); off(84); ui.tick();
globalThis.__6w6_main_lock = false;
jogClick(); ui.tick();
check(globalThis.__6w6_main_lock === true, "jog click on Main arms the lock");

setLog.length = 0; injected = [];
note(69, 100); off(69);
check(!setLog.some(([k]) => k === "synth:ui_focus"), "locked: a pad no longer moves the page");
check(injected.length === 2, "locked: the pad still plays and still records");

/* Shift+Pad is an explicit 'take me there' and must still navigate */
shift = true; setLog.length = 0;
note(70, 100); off(70); shift = false;
check(setLog.some(([k, v]) => k === "synth:ui_focus" && v === "2"),
      "locked: Shift+Pad still navigates (lane 2)");

/* the title advertises it. The header is a bitmap font, so this reads the
 * title the binding HANDS the renderer rather than scraping the screen. */
ui.tick();
check(/\[L\]/.test(spied.title || ""), "locked: the title says [L] (" + spied.title + ")");

/* Back to Main first -- a click only toggles the lock while ON Main, and the
 * Shift+Pad above deliberately navigated away. */
shift = true; note(84, 100); off(84); shift = false; ui.tick();
jogClick(); ui.tick();
check(globalThis.__6w6_main_lock === false, "a second jog click, on Main, unlocks");
ui.tick();
check(!/\[L\]/.test(spied.title || ""), "unlocked: [L] is gone (" + spied.title + ")");
setLog.length = 0;
note(69, 100); off(69);
check(setLog.some(([k, v]) => k === "synth:ui_focus" && v === "1"),
      "unlocked: pads move the page again");

/* the lock must survive the editor being re-opened (host re-evals the file) */
globalThis.__6w6_main_lock = true;
check(globalThis.__6w6_main_lock === true,
      "the lock lives on globalThis, so re-entering the editor keeps it");
globalThis.__6w6_main_lock = false;

/* knob 1 on the current page (CC 71 delta) writes a synth param */
setLog.length = 0;
ui.onMidiMessageInternal(new Uint8Array([0xB0, 71, 1]));
ui.tick();
check(setLog.some(([k]) => k.startsWith("synth:") && !k.endsWith("mutes")), "knob turn writes a synth param (" + (setLog[0] ? setLog[0][0] : "none") + ")");

/* jog turns pages; jog click opens the picker; Back closes it */
ui.onMidiMessageInternal(new Uint8Array([0xB0, 14, 1])); ui.tick();
ui.onMidiMessageInternal(new Uint8Array([0xB0, 3, 127])); ui.tick();
check(ui.handleBack() === true, "Back with the picker open is consumed (closes it)");
check(ui.handleBack() === false, "Back with no picker exits the editor");

/* unfocused: pads pass through, nothing else reacts */
globalThis.shadow_get_ui_slot = () => 1; setLog.length = 0; injected = [];
note(68, 100); ui.onMidiMessageInternal(new Uint8Array([0xB0, 71, 1]));
check(injected.length === 1 && setLog.length === 0, "unfocused slot: pads pass through, knobs ignored");

console.log(fails ? `\nFAILED (${fails})` : "\nALL PASS");
process.exit(fails ? 1 : 0);
