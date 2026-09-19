import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../src/ui_chain.js", import.meta.url), "utf8")
    .replace(/^import .*;\n/gm, "");

let tts = true;
let shift = false;
const spoken = [];
const layouts = [];
const writes = [];
const page = { level: "root", kind: "knobs" };

const controller = {
    pages: [page, { level: "bd", kind: "knobs" }],
    page,
    pickerOpen: false,
    knobRows() { return []; },
    load() { this.io.announce("6W6, 1 of 2, 7 controls"); },
    setLayout(layout) { layouts.push(layout); },
    showHint() { throw new Error("visual hint must not cover the accessible list"); },
    setReveal() {}, tick() {}, render() {}, goToPage() {},
    dismissHint() { return false; }, dismissPeek() { return false; },
    exitMenu() { return false; }, closePicker() {},
};

const context = vm.createContext({
    console,
    Date,
    LAYOUT_MOVY: "movy",
    PAGE_KNOBS: "knobs",
    PAGE_MENU: "menu",
    createController(io) { controller.io = io; return controller; },
    decodeInput(data, mods) {
        if ((data[0] & 0xF0) === 0xB0 && data[1] === 3 && data[2] > 0)
            return { type: "click", shift: !!mods.shift };
        return null;
    },
    applyInput() { return null; },
    tts_get_enabled() { return tts ? 1 : 0; },
    host_announce_screenreader(text) { spoken.push(String(text)); },
    shadow_get_ui_slot() { return 0; },
    shadow_get_display_mode() { return 1; },
    shadow_get_shift_held() { return shift ? 1 : 0; },
    shadow_get_param(slot, key) { return key === "synth:mutes" ? "0" : ""; },
    shadow_set_param(slot, key, value) { writes.push([slot, key, value]); },
    host_pad_block() {},
    move_midi_inject_to_move() {},
    clear_screen() {}, fill_rect() {}, print() {}, text_width() { return 0; },
});

vm.runInContext(source, context, { filename: "ui_chain.js" });
context.chain_ui.init();

assert.equal(layouts[0], "list", "TTS must select the accessible list layout");
assert.deepEqual(spoken.slice(0, 2), ["6W6", "6W6, 1 of 2, 7 controls"],
    "the actionable page announcement must follow the module name");

const expectedVoices = [
    [68, "Bass Drum"], [69, "Snare"], [70, "Low Tom"], [71, "High Tom"],
    [76, "Closed Hat"], [77, "Open Hat"], [78, "Cymbal"], [79, "Hand Clap"],
];
context.chain_ui.onMidiMessageInternal([0xB0, 88, 127]);
for (const [note, voice] of expectedVoices) {
    context.chain_ui.onMidiMessageInternal([0x90, note, 100]);
    assert.equal(spoken.at(-1), `${voice} muted`);
    context.chain_ui.onMidiMessageInternal([0x90, note, 100]);
    assert.equal(spoken.at(-1), `${voice} unmuted`);
}
assert.ok(writes.some(([, key]) => key === "synth:mutes"));

context.chain_ui.onMidiMessageInternal([0xB0, 88, 0]);
shift = true;
context.chain_ui.onMidiMessageInternal([0xB0, 3, 127]);
assert.equal(spoken.at(-1), "Main page locked");
context.chain_ui.onMidiMessageInternal([0xB0, 3, 127]);
assert.equal(spoken.at(-1), "Main page unlocked");

tts = false;
shift = false;
context.chain_ui.tick();
assert.equal(layouts.at(-1), "movy", "disabling TTS must restore the knob grid");

tts = true;
context.chain_ui.tick();
assert.equal(layouts.at(-1), "list",
    "enabling TTS while the editor is open must select the accessible list");

controller.knobRows = undefined;
context.chain_ui.tick();
assert.equal(layouts.at(-1), "movy",
    "an older controller without list support must keep the working knob grid");

console.log("PASS: accessible list, announcement order, all voice mutes, Main lock, and old-host fallback");
