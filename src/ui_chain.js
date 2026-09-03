/*
 * 6W6 ui_chain.js — a thin binding over Schwung's shared param_pages library
 * (host 0.12.1+), keeping every gesture the stock hierarchy editor does not
 * have. Ported from 9W9 with a 606 roster: the two kits must feel identical
 * under the hands, so the gestures, the hint and the focus gate are the same
 * code with different tables.
 *
 * Division of labour:
 *   param_pages (stock)          this file (6W6)
 *   ------------------          ---------------------------------
 *   knob grid, Movy layout      pads pass through to Move + page-follow
 *   viz graphics (faders...)    Shift+Pad silent select (white pad follows)
 *   jog page / Shift+Jog        Mute+Pad per-lane 6W6 mutes
 *   section picker (jog click)  focus gate (never steal another slot's pads)
 *   hold-knob name strip        pad_block lifecycle + self-heal
 *   Shift reveal + fine mode
 *   Mute+knob reset-to-default
 *
 * Why this file still exists at all: the host's enterComponentEdit prefers a
 * module's ui_hierarchy and would never load ui_chain.js if we served one — so
 * the DSP serves the hierarchy under "ui_pages" instead, and the injected
 * getParam below rewrites the controller's "ui_hierarchy" read to it. Pads are
 * not part of the stock grid's input model, so pad behaviour stays ours.
 *
 * GPL-3.0. param_pages © Schwung contributors; its level walk derives from
 * schwung-movy (MIT, megadake).
 */

import { createController } from '/data/UserData/schwung/shared/param_pages/page_controller.mjs';
import { decodeInput, applyInput } from '/data/UserData/schwung/shared/param_pages/page_input.mjs';
import { PAGE_KNOBS, PAGE_MENU } from '/data/UserData/schwung/shared/param_pages/page_plan.mjs';
import { LAYOUT_MOVY } from '/data/UserData/schwung/shared/param_pages/render_page_movy.mjs';

(function () {
    "use strict";

    /* raw pad note -> page key (page-follow). The left 4x4 block, counted
     * from the bottom-left: pads 1-4 are notes 68-71, 5-8 are 76-79, 9-12 are
     * 84-87. So the kit is the bottom two rows, and the row above it holds
     * the three page-only pads:
     *
     *   pad  9 (84)  Master     pad 10 (85)  Reverb     pad 11 (86)  Delay
     */
    var PAD2LEVEL = { 68: "bd", 69: "sd", 70: "lt", 71: "ht",
                      76: "ch", 77: "oh", 78: "cy", 79: "cp",
                      84: "root", 85: "rev", 86: "dly" };

    /* raw pad note -> 6W6 lane (Mute+Pad). Same order as the DSP's enum. */
    var PAD2LANE = { 68: 0, 69: 1, 70: 2, 71: 3, 76: 4, 77: 5, 78: 6, 79: 7 };

    /* page key -> what the remote panel should show. 0-7 are the voices,
     * 8 Master, 9 Reverb, 10 Delay. Separate from LEVEL2LANE below, which is
     * only about which lane's MUTE the title bar reflects. */
    var LEVEL2FOCUS = { bd: 0, sd: 1, lt: 2, ht: 3, ch: 4, oh: 5, cy: 6, cp: 7,
                        root: 8, rev: 9, dly: 10 };

    /* page key -> lane whose mute the title indicator shows (-1 = none) */
    var LEVEL2LANE = { bd: 0, sd: 1, lt: 2, ht: 3, ch: 4, oh: 5, cy: 6, cp: 7,
                       root: -1, rev: -1, dly: -1 };

    /* Pads that switch the page and never sound: they are not injected back
     * to Move, so Move never lights or records them. */
    function isPageOnlyPad(n) { return n === 84 || n === 85 || n === 86; }

    /* Main-page lock: a jog CLICK while already on the Main page toggles it.
     * While locked, pads still play and still record, but the page stops
     * following them -- so the master knobs stay under your hands while you
     * jam. Shift+Pad still navigates: that gesture is an explicit "take me
     * there". Another click unlocks; the title shows [L].
     *
     * It lives on globalThis deliberately. The host re-evaluates this file
     * every time the editor is opened, so module-level state would reset and
     * the lock would look like it dropped itself. */
    function mainLocked() { return !!globalThis.__6w6_main_lock; }

    function onMainPage() {
        var page = controller && controller.page;
        return !!page && (page.level === "root" || page.level == null);
    }

    var mySlot = -1;
    var padBlocked = false;
    var muteHeld = false;
    var editHeld = false;   /* Move's Delete or Copy held */
    var mutesMask = 0;
    var controller = null;

    /* First-run gesture hint: once per shadow_ui session, dismissed by any
     * input -- including pads, which are our layer -- or on its own after
     * HINT_MS. A hint nobody can wave away feels stuck. The "shown" flag lives
     * on globalThis: the host re-evaluates this file on every editor open, so
     * module-level state would reset each time; the shadow_ui process's global
     * object is what actually lives for the session. */
    var HINT_MS = 4000;
    var HINT_FLAG = "__6w6_hint_shown";
    var hintUntil = 0;

    function dismissHint() {
        hintUntil = 0;
        if (controller && controller.dismissHint) controller.dismissHint();
    }

    function has(fn) { return typeof globalThis[fn] === "function"; }

    function uiSlot() {
        return has("shadow_get_ui_slot") ? shadow_get_ui_slot() : 0;
    }

    function isFocused() {
        return mySlot >= 0 && uiSlot() === mySlot;
    }

    function setPadBlock(on) {
        if (padBlocked === on) return;
        if (has("host_pad_block")) { host_pad_block(on ? 1 : 0); padBlocked = on; }
    }

    function shiftHeld() {
        return has("shadow_get_shift_held") && !!shadow_get_shift_held();
    }

    /* The controller's device I/O. One special case: its "ui_hierarchy" read
     * is rewritten to "ui_pages" — the key the DSP actually serves, because
     * serving ui_hierarchy itself would stop this file from ever loading. */
    function ctlGetParam(key) {
        if (!has("shadow_get_param")) return null;
        if (key === "synth:ui_hierarchy") key = "synth:ui_pages";
        return shadow_get_param(mySlot, key);
    }

    function ctlSetParam(key, value) {
        if (has("shadow_set_param")) shadow_set_param(mySlot, key, String(value));
    }

    function announce(text) {
        if (has("host_announce_screenreader")) host_announce_screenreader(text);
    }

    function refreshMutes() {
        var m = parseInt(ctlGetParam("synth:mutes"), 10);
        mutesMask = isNaN(m) ? 0 : m;
    }

    function toggleLaneMute(lane) {
        mutesMask = (mutesMask ^ (1 << lane)) & 0xFF;
        ctlSetParam("synth:mutes", String(mutesMask));
    }

    /* Jump the grid to the first page of a hierarchy level, and publish the
     * page id so the remote panel follows. A set_param, not a read: the shim
     * only tells schwung-manager about WRITES. */
    var lastFocus = -1;
    function goToLevel(levelKey) {
        if (!controller) return;
        /* Publishing the page is the TICK's job, not this function's: the
         * page also moves by jog, and doing it in one place means it is
         * announced however it moved. */
        var pages = controller.pages;
        for (var i = 0; i < pages.length; i++) {
            if (pages[i].level === levelKey ||
                (levelKey === "root" && pages[i].level === null)) {
                controller.goToPage(i);
                return;
            }
        }
        if (levelKey === "root") controller.goToPage(0);
    }

    /* Give the pad back to Move as a real press: plays the (HiJack-muted) kit,
     * records while REC is on, and updates Move's pad selection. */
    function injectToMove(data) {
        if (!has("move_midi_inject_to_move")) return;
        var type = (data[0] & 0xF0) === 0x90 ? 0x09
                 : (data[0] & 0xF0) === 0x80 ? 0x08
                 : (data[0] & 0xF0) === 0xA0 ? 0x0A : 0;
        if (!type) return;
        move_midi_inject_to_move([type, data[0], data[1], data[2]]);
    }

    /* ---------------- chain_ui hooks ---------------- */

    function init() {
        mySlot = uiSlot();
        setPadBlock(true);
        refreshMutes();

        controller = createController({
            getParam: ctlGetParam,
            setParam: ctlSetParam,
            announce: announce,
            /* The host's trailing "My Presets" and "Module" pages. They are the
             * HOST's to build -- the preset record lives in the slot config,
             * not in us -- and it hands them over as menu pages here.
             *
             * Guarded twice over: the binding is absent on a host without
             * charlesvestal/schwung#396, and absent for a Master FX position,
             * which has no preset record. Either way the array is empty,
             * nothing is appended, and the editor behaves exactly as before. */
            trailingMenus: function () {
                return (typeof shadow_component_trailing_menus === "function")
                    ? (shadow_component_trailing_menus() || [])
                    : [];
            }
        });
        controller.load({ slot: mySlot, component: "synth", prefix: "synth" });
        controller.setLayout(LAYOUT_MOVY);
        if (!globalThis[HINT_FLAG]) {
            globalThis[HINT_FLAG] = true;
            controller.showHint([
                "Pad: play + select",
                "Sh+Pad: select only",
                "Mute+Pad: mute drum",
                "Jog click on Main: lock",
                "Jog: page  Click: list",
                "Shift: fine + values",
                "Mute+knob: default"
            ], "6W6");
            hintUntil = Date.now() + HINT_MS;
        }
        announce("6W6");
    }

    /* Title-bar text. The stock grid prints the page's own name on the right
     * of the bar, so this must NOT repeat it ("6W6 > BASS DRUM  BASS DRUM"):
     * just the module name plus the mute flag for the drum on screen. */
    function title() {
        var t = "6W6";
        if (mainLocked()) t += " [L]";
        var page = controller && controller.page;
        var lane = page ? LEVEL2LANE[page.level] : -1;
        if (lane !== undefined && lane >= 0 && (mutesMask & (1 << lane)))
            t += " [M]";
        return t;
    }

    function tick() {
        var shown = !has("shadow_get_display_mode") || shadow_get_display_mode() === 1;
        var active = shown && isFocused();
        setPadBlock(active);
        if (!active || !controller) return;

        if (hintUntil && Date.now() >= hintUntil) dismissHint();
        controller.setReveal(shiftHeld());
        /*
         * The grid's own housekeeping: one value read per tick around the
         * page, plus the modulation flags and any late metadata. Without it
         * `values` stays empty forever -- knobs fall back to 0 and enum boxes
         * render blank, which is exactly what dropping this line caused.
         */
        controller.tick();

        /*
         * Publish whichever page is on screen, however it got there -- pad or
         * jog. The remote panel HIGHLIGHTS the voice you are on; it does not
         * follow it, because every control is visible there at once and there
         * is no page to switch. So this is one-way and one write per page
         * change, nothing more.
         */
        var focusNow = LEVEL2FOCUS[controller.page ? controller.page.level : null];
        if (focusNow === undefined) focusNow = 8;
        if (focusNow !== lastFocus) {
            lastFocus = focusNow;
            ctlSetParam("synth:ui_focus", String(focusNow));
        }

        /* The grid paces its own redraws; draw every tick like the stock
         * binding does (a full page render is ~1.6 ms, measured upstream). */
        clear_screen();
        var page = controller.page;
        /* PAGE_MENU as well as the grid. A menu page is a list of actions with
         * no params behind it and the LIBRARY draws it; excluding it here would
         * print our unsupported-page fallback over a page param_pages was about
         * to draw correctly. This is what the host's trailing "My Presets" and
         * "Module" pages arrive as. */
        if (controller.pickerOpen ||
            (page && (page.kind === PAGE_KNOBS || page.kind === PAGE_MENU))) {
            controller.render(
                {
                    fillRect: fill_rect, print: print, textWidth: text_width,
                    line: typeof draw_line === "function" ? draw_line : undefined,
                    fillCircle: typeof fill_circle === "function" ? fill_circle : undefined,
                    drawCircle: typeof draw_circle === "function" ? draw_circle : undefined,
                    drawArc: typeof draw_arc === "function" ? draw_arc : undefined
                },
                { title: title() }
            );
            /*
             * THE SECOND HALF OF THE DRAW, and it is not optional.
             *
             * render() paints a page into a rect the CALLER owns; nothing in
             * param_pages clears the screen, which is what lets a consumer
             * host a page inside its own chrome. So anything FULL-SCREEN is
             * handed back to the frame owner -- and that is us.
             *
             * Today that means the enum peek: turn a multi-option enum and its
             * option list rises over the grid for ~700ms. Without this call
             * the controller still tracks the peek and applyInput still
             * swallows the Back that dismisses it; it is simply painted
             * nowhere. That is how 6W6 shipped, silently, from the first
             * release: every enum on every page showed one word and never the
             * list.
             *
             * Guarded because renderOverlays landed in a later host than this
             * file's min_host_version, and an older host simply has no
             * overlays to draw.
             */
            if (typeof controller.renderOverlays === "function") {
                controller.renderOverlays(
                    { fillRect: fill_rect, print: print, textWidth: text_width },
                    { clearScreen: clear_screen }
                );
            }
        } else {
            /* Non-grid page kinds do not occur in 6W6's hierarchy; if one ever
             * does, show something honest instead of a stale frame. */
            print(2, 28, "6W6: unsupported page", 1);
        }
    }

    function onMidiMessageInternal(data) {
        var status = data[0] & 0xF0;
        var d1 = data[1];
        var d2 = data[2];

        /* Another slot is focused: never react; keep the surface alive. */
        if (!isFocused()) {
            if ((status === 0x90 || status === 0x80 || status === 0xA0) &&
                d1 >= 68 && d1 <= 99)
                injectToMove(data);
            return;
        }

        /* Mute button held-state (CC 88): ours for Mute+Pad, the library's
         * for Mute+knob reset — tracked here, passed to decodeInput below. */
        if (status === 0xB0 && d1 === 88) {
            muteHeld = (d2 > 0);
            return;
        }

        /* Delete (CC 119) and Copy (CC 60) are MOVE's gestures, not ours.
         * While either is held we get out of the way entirely -- see the pad
         * branch below. */
        if (status === 0xB0 && (d1 === 119 || d1 === 60)) {
            editHeld = (d2 > 0);
            injectToMove(data);
            return;
        }

        /* ---- Pads: 6W6's own layer ---- */
        if ((status === 0x90 || status === 0x80 || status === 0xA0) &&
            d1 >= 68 && d1 <= 99) {

            /*
             * Move's own pad gestures win. Delete+Pad and Copy+Pad address
             * Move's CLIP, not our kit, and our page-only pads (Master,
             * Reverb, Delay) are normally swallowed -- which left steps
             * recorded on those pads by another module impossible to select
             * or delete, because the press never reached Move. While Delete
             * or Copy is held, every pad passes straight through untouched:
             * no page follow, no mute toggle, no silent-select window.
             */
            if (editHeld) {
                injectToMove(data);
                return;
            }

            if (status === 0x90 && d2 > 0) dismissHint();

            /* Mute + Pad: toggle that lane's 6W6 mute; press still reaches
             * Move so its native state stays in step. */
            if (muteHeld) {
                var lane = PAD2LANE[d1];
                if (status === 0x90 && d2 > 0 && lane !== undefined)
                    toggleLaneMute(lane);
                if (!isPageOnlyPad(d1)) injectToMove(data);
                return;
            }

            var level = PAD2LEVEL[d1];

            /* Locked to Main: the pad plays, the page stays. */
            if (mainLocked() && !shiftHeld()) {
                if (!isPageOnlyPad(d1)) injectToMove(data);
                return;
            }

            if (shiftHeld()) {
                /* Silent select: page follows AND Move's white pad follows —
                 * the DSP swallows exactly the one note routed back (60 ms
                 * window). Accepted trade: with REC armed and playing, this
                 * press would be recorded; Gus does not use REC. */
                if (status === 0x90 && d2 > 0) {
                    if (level !== undefined) goToLevel(level);
                    if (!isPageOnlyPad(d1)) {
                        ctlSetParam("synth:mute_ms", "60");
                        injectToMove(data);
                    }
                } else {
                    if (!isPageOnlyPad(d1)) injectToMove(data);   /* release */
                }
                return;
            }

            /* Plain pad: page follows what you play; Move plays/records.
             * PAD2LEVEL already maps the page-only pads to their pages, so
             * one branch covers both kinds. */
            if (status === 0x90 && d2 > 0 && level !== undefined)
                goToLevel(level);
            if (!isPageOnlyPad(d1)) injectToMove(data);
            return;
        }

        /* ---- Everything else: the stock grid's input model ---- */
        if (!controller) return;
        var intent = decodeInput(data, { shift: shiftHeld(), mute: muteHeld });
        if (!intent) return;
        /* SHIFT + jog click toggles the Main-page lock. Before applyInput, or
         * the section picker consumes it.
         *
         * A PLAIN click is Schwung's and has to stay Schwung's: it opens the
         * section list, and on the host's trailing pages it is how a row is
         * activated -- Save As, Delete, Swap Module. This took the plain click
         * while Main was ours alone; that stopped being true the moment the
         * host began appending its own pages to us. Taking a gesture the
         * platform needs is not ours to do. */
        if (intent.type === "click" && shiftHeld() &&
            !controller.pickerOpen && onMainPage()) {
            globalThis.__6w6_main_lock = !globalThis.__6w6_main_lock;
            return;
        }
        var todo = applyInput(controller, intent, { nowMs: Date.now(), reveal: false });
        if (todo && todo.action === "exit") {
            /* Back never reaches us (the host consumes it); any other exit
             * intent just closes the picker. */
            if (controller.pickerOpen) controller.closePicker();
        }
        /* A menu row was activated. The controller reports it as
         * { action: "menu", entry } -- the ENTRY carries the action key
         * (up_save_as, swap_module ...); "menu" is only the intent's KIND.
         * Handing the host the word "menu" runs nothing, silently, and that is
         * exactly what 9W9's first cut did. Performed by the shadow UI, not
         * here: those keys reach the preset store, the browser, the component
         * picker and the help screen, none of which a module can address. */
        if (todo && todo.action === "menu") {
            var act = todo.entry && todo.entry.action;
            if (act && typeof shadow_component_run_action === "function")
                shadow_component_run_action(act);
            return;
        }
        /* 'open' (opaque param editors) cannot occur: every 6W6 param is an
         * int or an enum. Ignored if a future param ever produces one. */
    }

    function onMidiMessageExternal(data) { }

    /* The host consumes Back and asks us FIRST, so we have to climb the same
     * rungs page_input.mjs's own `case "back"` does, in its order -- hint,
     * peek, picker, menu -- or Back means something different here than on
     * every stock grid. Without the menu rung, Back from inside My Presets
     * leaves the module entirely and skips the page bar. */
    function handleBack() {
        if (!controller) { setPadBlock(false); return false; }
        if (controller.dismissHint && controller.dismissHint()) return true;
        if (controller.dismissPeek && controller.dismissPeek()) return true;
        if (controller.pickerOpen) { controller.closePicker(); return true; }
        if (controller.exitMenu && controller.exitMenu()) return true;  /* the menu, not the module */
        setPadBlock(false);
        return false;                          /* host exits the editor */
    }

    globalThis.chain_ui = {
        init: init,
        tick: tick,
        onMidiMessageInternal: onMidiMessageInternal,
        onMidiMessageExternal: onMidiMessageExternal,
        handleBack: handleBack,
        /* A preset was saved or loaded while our grid is on screen. The
         * "My Presets" row is built by OUR controller out of the host's menus,
         * so nothing else would refresh it and it would go on reading "(none)"
         * after a Save. Re-plans the trailing pages only. */
        onPresetsChanged: function () {
            if (controller && typeof controller.refreshTrailing === "function")
                controller.refreshTrailing();
        },
        /* After Load, Delete, Swap or Help the host reloads us and says which
         * page we left from and whether to land inside its menu. The controller
         * keeps the request armed until its pages arrive, so a contract still
         * settling after the reload is fine. Without this every return lands
         * on Main. */
        restorePage: function (name, opts) {
            if (controller && typeof controller.restorePage === "function")
                controller.restorePage(name, opts || {});
        }
    };
})();
