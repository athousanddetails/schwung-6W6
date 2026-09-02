#!/usr/bin/env python3
"""Single source of truth for 6W6's parameter surface.

Emits src/dsp/sd606_params.h, which carries THREE things that must never drift
apart:

  * chain_params   — types/ranges/options the Shadow UI needs (JSON)
  * ui_pages       — the page hierarchy (JSON; served under "ui_pages" so
                     ui_chain.js loads, see the comment in ui_chain.js)
  * the pot table  — key -> real engineering range + curve + default

9W9 kept the pot table hand-written in a second header and it was a standing
drift hazard. Here one Python dict generates all three, so adding a control is
one edit and the loadtest's key list is derived from the same source.

module.json is capped at 8 KB by Schwung's loader, so the JSON payloads are
served dynamically from the DSP via get_param().
"""
import json, pathlib

LIN, EXP = 0, 1

# Every continuous control is a 0..127 pot, exactly like a hardware panel.
# Nobody dials a 606 in milliseconds. The DSP maps each pot to its real range
# with a musical curve, so the UI only ever shows a pot position.
#
# Tune pots are CENTRED on the value the upstream repo fitted from hardware:
# pot 64 == the measured 606 tuning, so "centre" is always the stock sound.
#
#   key, label, min, max, curve, default_pot
def P(key, label, lo, hi, curve, default):
    return dict(kind="pot", key=key, name=label, min=lo, max=hi,
                curve=curve, default=default)

def E(key, label, options, default=0):
    return dict(kind="enum", key=key, name=label, options=options,
                default=default)

# Post-voice drive stage. The vendored 606 voices have no distortion of their
# own (only internal saturation), so this is ours — same four flavours as 9W9
# so the two kits behave identically under the same knob.
# Option text is sized for the stock grid's enum box: TWO LINES OF THREE
# CHARACTERS (two words -> first 3 of each; one word -> chars 0-3 / 3-6,
# font5x3.mjs enumSquareLines). "Hard Clip" rendered HAR/CLI and "Wavefolder"
# WAV/EFO; these read DIO/DE, CLI/P, FOL/D, CRU/SH. Order is storage order.
DIST = ["Diode", "Clip", "SAT", "BFZ", "PDIST", "Fold", "Crush"]
# Option text is sized for the stock grid's enum box: TWO LINES OF THREE
# CHARACTERS (font5x3.mjs enumSquareLines) -- DIO/DE, CLI/P, SAT, BFZ, PDI/ST,
# FOL/D, CRU/SH. "SAT"/"BFZ"/"PDIST" were named to fit; do not rename them.
#
# ORDER IS STORAGE ORDER, and it changed at state v2: Fold moved 2->5 and
# Crush 3->6. sd606_deserialize remaps old blobs. Never reorder again without
# extending that migration.
# Drive: (0.85, 12) EXP, ported from 9W9. The old (0.2, 8) put unity at pot
# 55, so the bottom 43% of the knob only ATTENUATED and distortion "only kinda
# kicked in around 53". This range starts transparent (pot 8 == 1.0043) and
# climbs immediately. Changing it changes what every STORED pot position
# means, which is why state v2 re-solves them -- see sd606_deserialize.
def DRIVE(v):  return P(f"{v}_drive", "Drive", 0.85, 12.0, EXP, 8)   # pot 8 ~= 1.0
def DTYPE(v):  return E(f"{v}_dist_type", "Distortion", DIST)
def LEVEL(v):  return P(f"{v}_level", "Level", 0.0, 2.0, LIN, 64)   # pot 64 == 1.0
# Pitch RATIO pots: 0.5..2.0 exponential puts unity dead centre at pot 64.
def RATIO(v, label="Tune"): return P(f"{v}_tune", label, 0.5, 2.0, EXP, 64)
# The repo's decay arguments are already normalised 0..1.
def DECAY(v, default): return P(f"{v}_decay", "Decay", 0.0, 1.0, LIN, default)

# ---- Pages. One page per voice, in TR-606 front-panel order. --------------
# DEFAULTS are fitted, not chosen: tools/fit_defaults.cpp searches each voice's
# pots against the matching hardware recording through the real engine. Decays
# and the kick/snare come from that. Tune pots stay at 64 wherever measured
# pitch already matched the hardware -- the fitter will trade pitch for
# spectral fill, and pitch is the one thing we can measure directly.
# The 606's own roster is BD/SD/LT/HT/CY/OH/CH. Clap is the one addition —
# the 606 never had one, but the repo ships an RD-6 fitted clap and a spare
# pad is a spare pad.
PAGES = [
    ("bd", "Bass Drum", [
        # BassDrumVoice::trigger(transient, decay, tuningSemitones, variation).
        # Tuning is in semitones, centred on the repo's fitted 2.22.
        # Defaults FITTED against a hardware 606 kick (tools/fit_defaults,
        # 2720 renders): the upstream defaults are its "XL 608" kick, which
        # rings 9x longer than a 606 (-20 dB at 770 ms vs 85 ms). Score vs the
        # recording 20.3 -> 7.7. Tune -2.2 st, decay 0.19, attack 0.94.
        P("bd_tune",    "Tune",    -9.78, 14.22, LIN, 40),
        DECAY("bd", 24),
        P("bd_attack",  "Attack",   0.0,  1.0,  LIN, 120),  # transient amount
        DRIVE("bd"), DTYPE("bd"), LEVEL("bd"),
    ]),
    ("sd", "Snare", [
        # SnareVoice::trigger(decay, bodyPitchRatio, snappy, noiseColorRatio)
        # Defaults FITTED against the hardware snare (1680 renders, score
        # 11.3 -> 8.4): a touch sharper, shorter, less wire, darker wire.
        P("sd_tune",    "Tune",     0.5,  2.0,  EXP, 70),
        P("sd_snappy",  "Snappy",   0.0,  1.0,  LIN, 64),   # wire level only
        P("sd_tone",    "Tone",     0.5,  2.0,  EXP, 52),   # noise colour ratio
        DRIVE("sd"), DTYPE("sd"), LEVEL("sd"),
    ]),
    ("lt", "Low Tom", [
        RATIO("lt"), DECAY("lt", 124), DRIVE("lt"), DTYPE("lt"), LEVEL("lt"),   # decay fitted vs hardware
    ]),
    ("ht", "Hi Tom", [
        RATIO("ht"), DECAY("ht", 124), DRIVE("ht"), DTYPE("ht"), LEVEL("ht"),   # decay fitted vs hardware
    ]),
    ("ch", "Closed Hat", [
        RATIO("ch"), DECAY("ch", 102),                      # 0.8 == as measured on the hardware
        DRIVE("ch"), DTYPE("ch"), LEVEL("ch"),
        # Lives here as well as on Master: this is where you are standing when
        # you want it. The 606 hardwires CH>OH; here it is a switch.
        E("hh_choke", "Choke", ["Off", "CH>OH", "Mutual"], 1),   # CH>/OH in the box
    ]),
    ("oh", "Open Hat", [
        RATIO("oh"), DECAY("oh", 102), DRIVE("oh"), DTYPE("oh"), LEVEL("oh"),   # 0.8 == as measured
    ]),
    ("cy", "Cymbal", [
        RATIO("cy"), DECAY("cy", 102), DRIVE("cy"), DTYPE("cy"), LEVEL("cy"),   # 0.8 == as measured
    ]),
    ("cp", "Clap", [
        # ClapVoice::trigger(decay, pitchRatio, noiseAmount); 0.5 is the fit.
        RATIO("cp"), DECAY("cp", 102),
        P("cp_noise", "Noise", 0.0, 1.0, LIN, 64),
        DRIVE("cp"), DTYPE("cp"), LEVEL("cp"),
    ]),
]

# Send amounts, one pair per voice. Post-fader, 0 by default -- and that
# default is load-bearing: at zero the FX ticks see exactly 0.0 and return
# exactly 0.0, so the kit is bit-identical to one with no FX at all.
def SENDS(v): return [P(f"{v}_rev", "Rev", 0.0, 1.0, LIN, 0),
                      P(f"{v}_dly", "Dly", 0.0, 1.0, LIN, 0)]

# Tempo-synced delay: Time is a note DIVISION, not milliseconds, so it is an
# enum. Order must match kSd606DlyBeats in sd606_fx.h.
DIVS = ["1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.",
        "1/4", "1/2T", "1/4.", "1/2", "1/2."]

FX_PAGES = [
    ("rev", "Reverb", [
        P("rev_decay", "Decay",  0.2,  0.93, LIN, 73),      # 0.62
        P("rev_tone",  "Tone",   0.0,  1.0,  LIN, 57),      # 0.45
        P("rev_hpf",   "HPF",   30.0, 800.0, EXP, 62),      # 150 Hz
        P("rev_level", "Level",  0.0,  1.2,  LIN, 85),      # 0.80
    ]),
    ("dly", "Delay", [
        E("dly_time",  "Time", DIVS, 7),                    # dotted eighth
        P("dly_fdbk",  "Fdbk",   0.0,  0.85, LIN, 52),      # 0.35
        P("dly_tone",  "Tone",   0.0,  1.0,  LIN, 51),      # 0.40
        P("dly_hpf",   "HPF",   30.0, 800.0, EXP, 62),      # 150 Hz
        P("dly_level", "Level",  0.0,  1.2,  LIN, 85),      # 0.80
    ]),
]

GLOBALS = [
    E("master_dist", "Master Dist", ["Off"] + DIST),
    P("master_drive", "Master Drive", 0.85, 12.0, EXP, 8),   # same range as the voices
    # 0.63 at centre-ish, not 0.8: a dense accented pattern peaks around
    # -1.1 dBFS from here, so the kit leaves real headroom for whatever
    # the chain puts after it instead of arriving already clipped.
    P("volume", "Volume", 0.0, 1.0, LIN, 76),
    # No Accent pot. Velocity replaced it: accent WAS the level a hard hit
    # reached, and that is now simply the top of the velocity range. The gain
    # it used to contribute is folded into SD606_FULL_VELOCITY_GAIN so nothing
    # gets quieter -- deleting it and anchoring at 1.0 would drop the whole kit
    # 6 dB, because 1.0 is the UNACCENTED level and a pattern from Move (which
    # sends velocity 100 and up) had always been playing at the accented one.
    # One-knob bus glue, ported from 9W9. NOT 606 circuitry and honest about
    # it: at zero the stage is not in the path at all (bit-identical), and the
    # knob blends threshold, ratio and auto-makeup together.
    P("comp", "Comp", 0.0, 1.0, LIN, 0),
    # Velocity depth, ported from 9W9. The 606's accent is a per-step SWITCH,
    # one bus at one level, so the engine did the same and every note under
    # the accent point came out identical -- which is why hat grooves were
    # flat. This blends in a continuous law BELOW the accent point only; at 1
    # (the default, velocity live) gain is velocity/100, so a note at 99 sits
    # where it always did and the dynamics open up underneath. Accented notes
    # keep the accent gain exactly, at every depth.
    P("vel_depth", "Velocity", 0.0, 1.0, LIN, 127),
    E("hh_choke", "Choke", ["Off", "CH>OH", "Mutual"], 1),   # CH>/OH in the box
    E("note_map", "Note Map", ["Rack 36", "GM"]),            # RAC/36 in the box
]

# ---------------------------------------------------------------------------
def viz_for(p):
    """Honest viz declarations for the 0.12.x param-pages renderer.

    Levels draw as faders. "Attack" is the kick's CLICK LEVEL, not an envelope
    time — declare viz:false so the detector cannot pair it with Decay into a
    fake AD envelope (9W9 learned this the hard way)."""
    k = p["key"]
    if k.endswith("_level") or k == "volume":
        return {"kind": "fader"}
    if k.endswith("_attack"):
        return False
    return None

# Schwung's LFO / modulation picker lists a module's parameters as ONE FLAT
# LIST, so eight voices each contributing a "Decay" gave eight identical rows
# and nothing said which pad you were about to automate. chain_params `name`
# is what that picker shows, so it carries a prefix.
#
# The prefixes are the machine's own two-letter panel legend -- BD SD LT HT CY
# OH CH, exactly as they are silk-screened on a 606 -- plus CP for the clap the
# 606 never had, and REV/DLY for the two buses.
#
# The PAGE label must stay short ("Decay"), because the knob grid squeezes it
# into about five characters. Those are different fields: param_meta.mjs
# resolves `label || name || key`, so the hierarchy entry supplies `label` and
# wins on the page, while chain_params supplies the prefixed `name` and wins in
# the picker. Emitting `name` on the hierarchy entry -- which is what it used
# to do, and what validate_contract.mjs asks for -- makes the prefix leak onto
# the page instead.
PICKER_PREFIX = {"bd": "BD", "sd": "SD", "lt": "LT", "ht": "HT",
                 "ch": "CH", "oh": "OH", "cy": "CY", "cp": "CP",
                 "rev": "REV", "dly": "DLY"}

def picker_name(key, name):
    head = key.split("_", 1)[0]
    pre = PICKER_PREFIX.get(head)
    return f"{pre} {name}" if pre else name


def chain_param(p):
    if p["kind"] == "enum":
        d = {"key": p["key"], "name": picker_name(p["key"], p["name"]),
             "type": "enum", "options": p["options"]}
    else:
        d = {"key": p["key"], "name": picker_name(p["key"], p["name"]),
             "type": "int", "min": 0, "max": 127}
    # The fitted default, so a reset gesture (stock Mute+knob, Movy, the web
    # panel's double-click) lands on the 606 and not on a guessed 64.
    d["default"] = p["default"]
    v = viz_for(p)
    if v is not None:
        d["viz"] = v
    return d

cp, levels, root, pots, enums, seen = [], {}, [], [], [], set()

def register(p):
    if p["key"] in seen:
        return
    seen.add(p["key"])
    cp.append(chain_param(p))
    if p["kind"] == "pot":
        pots.append(p)
    else:
        enums.append(p)

# THE POT AND ENUM TABLES ARE POSITIONAL STORAGE. The state blob holds raw
# values by index, so a key may be APPENDED but never inserted or reordered.
# That is why registration below is deliberately not in page order: every
# parameter that existed before the send FX is registered first, in exactly
# its old order, and the new sends and FX params go on the end. Pages may then
# list them in whatever order reads well.
PAGE_SENDS = {pid: SENDS(pid) for pid, _, _ in PAGES}

# bd_drift is GONE, not hidden: the 606's kick has no per-hit pitch variation,
# and the pot had always defaulted to 0, so removing it changes no sound --
# only the ability to turn it on.
#
# Deleting it RENUMBERS the positional pot table, which is why v1 blobs are
# migrated BY NAME rather than by index -- see kV1PotKeys in sd606_engine.cpp.

# The snare's decay came back by popular demand. The machine has no such
# control, so it is not "restored" to a front-panel position -- it is added,
# defaulted to the value that WAS pinned (pot 76, fitted against the hardware
# recording), so every existing patch plays exactly as it did and the control
# is purely opt-in. SnareVoice gates the body AND the wires, so it shortens
# the whole drum, not just the tail. Measured audible tail (tools probe, -80
# dBFS): 2.5 ms at pot 0, 57 ms at 32, 141 ms at the default, 370 ms wide open.
#
# It is registered LAST, not in the Snare page's list, because the pot table is
# APPEND-ONLY -- inserting it among the originals would renumber every pot
# after it and load old patches as a different kit.
SD_DECAY = DECAY("sd", 76)
LATE = [SD_DECAY]

# Knobs the device page SUBSTITUTES. A Move page is exactly 8 encoders and the
# Snare page was full, so Decay takes Tone's position. sd_tone is still a real
# parameter -- registered, in chain_params, on the web panel and available as
# an LFO target -- it just no longer spends one of the eight.
PAGE_SWAP = {"sd_tone": SD_DECAY}

for pid, label, params in PAGES:          # 1. the original per-voice params
    for p in params:
        register(p)
for p in GLOBALS:                         # 2. the original globals
    register(p)
for pid, _, _ in PAGES:                   # 3. NEW: the send pots
    for p in PAGE_SENDS[pid]:
        register(p)
for _, _, params in FX_PAGES:             # 4. NEW: the FX params
    for p in params:
        register(p)
for p in LATE:                            # 5. NEWER STILL: appended, never inserted
    register(p)

# Now the pages, which may reference anything registered above.
for pid, label, params in PAGES:
    full = [PAGE_SWAP.get(p["key"], p) for p in params] + PAGE_SENDS[pid]
    if len(full) > 8:
        raise SystemExit(f"page {pid} has {len(full)} params -- max 8 knobs")
    levels[pid] = {"name": label,
                   "knobs": [p["key"] for p in full],
                   "params": [{"key": p["key"], "label": p["name"]} for p in full]}
    root.append({"level": pid, "label": label})

for pid, label, params in FX_PAGES:
    levels[pid] = {"name": label,
                   "knobs": [p["key"] for p in params],
                   "params": [{"key": p["key"], "label": p["name"]} for p in params]}
    root.append({"level": pid, "label": label})

root += [{"key": p["key"], "label": p["name"]} for p in GLOBALS]
levels["root"] = {"name": "6W6",
                  "knobs": ["master_dist", "master_drive", "comp", "volume", "vel_depth"],
                  "params": root}

# Two plugin-level keys that live on NO page but must be in chain_params: the
# remote-UI manager seeds and periodically re-reads exactly the keys listed
# here, and a key it does not know about never reaches the browser. ui_focus
# is the lane the on-device editor is showing (0-7, 8 = master); mutes is the
# per-lane mute mask. Both are served by the plugin, not the pot table.
cp.append({"key": "ui_focus", "name": "Focus", "type": "int", "min": 0, "max": 8, "default": 0})
cp.append({"key": "mutes", "name": "Mutes", "type": "int", "min": 0, "max": 255, "default": 0})

cpj = json.dumps(cp, separators=(",", ":"))
uhj = json.dumps({"levels": levels}, separators=(",", ":"))

def cstr(s):
    q, b = chr(34), chr(92)
    return "\n".join(
        f'    "{s[k:k+100].replace(b, b*2).replace(q, b+q)}"'
        for k in range(0, len(s), 100))

pot_rows = "\n".join(
    f'    {{ "{p["key"]}", {p["min"]:>10.4f}f, {p["max"]:>10.4f}f, '
    f'{"SD606_EXP" if p["curve"] == EXP else "SD606_LIN"}, {p["default"]:>3} }},'
    for p in pots)
enum_rows = "\n".join(
    f'    {{ "{p["key"]}", {len(p["options"]):>2}, {p["default"]:>2} }},'
    for p in enums)

root_dir = pathlib.Path(__file__).resolve().parent.parent
(root_dir / "src/dsp/sd606_params.h").write_text(f"""/* Generated by scripts/gen_params.py — DO NOT EDIT BY HAND.
 *
 * module.json is capped at 8 KB by Schwung's loader, so chain_params and the
 * page hierarchy are served dynamically from the DSP via get_param().
 *
 * The pot and enum tables below define storage order for the state blob.
 * Appending is safe; reordering breaks every saved patch.
 */
#ifndef SD606_PARAMS_H
#define SD606_PARAMS_H

typedef enum {{ SD606_LIN = 0, SD606_EXP = 1 }} sd606_curve_t;

typedef struct {{
    const char   *key;
    float         min;
    float         max;
    sd606_curve_t curve;
    int           def;      /* default POT position, 0..127 */
}} sd606_pot_t;

typedef struct {{
    const char *key;
    int         count;      /* number of options */
    int         def;
}} sd606_enum_t;

#define SD606_NUM_POTS  {len(pots)}
#define SD606_NUM_ENUMS {len(enums)}

static const sd606_pot_t g_sd606_pots[SD606_NUM_POTS] = {{
{pot_rows}
}};

static const sd606_enum_t g_sd606_enums[SD606_NUM_ENUMS] = {{
{enum_rows}
}};

#define SD606_CHAIN_PARAMS_LEN {len(cpj)}
static const char sd606_chain_params_json[] =
{cstr(cpj)};

#define SD606_UI_PAGES_LEN {len(uhj)}
static const char sd606_ui_pages_json[] =
{cstr(uhj)};

#endif /* SD606_PARAMS_H */
""")
# ---- movy_config.json: same source, Movy's shape. ---------------------------
# HARD RULE (cost a debugging session on Tablor): a Movy bank is EXACTLY ONE
# PAGE. buildConfigPages keys bankGroups per BANK but the UI indexes per PAGE,
# so a multi-row bank shifts every following page's label. One row per bank.
SHORT = {"Velocity": "VEL", "Comp": "COMP", "Rev": "REV", "Dly": "DLY", "Fdbk": "FDBK", "HPF": "HPF", "Time": "TIME",
         "Tune": "TUNE", "Decay": "DECAY", "Attack": "ATTK", "Drift": "DRIFT",
         "Drive": "DRIVE", "Distortion": "DIST", "Level": "LEVEL",
         "Snappy": "SNAPY", "Tone": "TONE", "Noise": "NOISE", "Choke": "CHOKE",
         "Master Dist": "MDIST", "Master Drive": "MDRV", "Volume": "VOL",
         "Accent": "ACNT", "Note Map": "NMAP"}
MOVY_NAME = {"rev": "Reverb", "dly": "Delay", "bd": "Kick", "sd": "Snare", "lt": "Lo Tom", "ht": "Hi Tom",
             "ch": "Cl Hat", "oh": "Op Hat", "cy": "Cymbal", "cp": "Clap"}
def movy_slot(p):
    d = {"key": p["key"], "short": SHORT[p["name"]], "full": p["name"]}
    if p["kind"] == "enum":
        d["type"] = "enum"
        # Movy squeezes option text into a 32 px cell: short words only.
        d["options"] = list(p["options"])   # already sized for a 32 px cell
    else:
        d["type"] = "int"; d["min"] = 0; d["max"] = 127
    return d
# PAD-FOLLOWS-PAGE. A bank carrying "pad" is selected when that Move pad is
# pressed, the way the device editor already switches page on a pad. This
# MIRRORS src/ui_chain.js's PAD2LEVEL rather than inventing a second layout --
# the two surfaces must agree or the same pad means two different things.
#
#   pads 1-8   the eight voices, in drum-rack order
#
# Movy pad numbers are 1-BASED and keyed to the NOTE, not the grid seat:
# pad N is padNoteStart + N - 1, so pad 1 is note 36 (bd).
#
# NO PAGE-ONLY PADS. An earlier revision put Master on pad 9 and the sends on
# 10 and 11 -- spare seats past the kit's notes that sound nothing and just
# turn the page, mirroring isPageOnlyPad() in our own editor. Movy takes the
# LEADING run of pad-declaring banks as the voices, so those three pads made
# Master/Reverb/Delay read as voices and took them out of the page rotation
# entirely. Pages without a voice get their own seat in the page list instead;
# in Movy pads 9..16 are simply dead.
PAD_OF_LEVEL = {pid: i + 1 for i, (pid, _, _) in enumerate(PAGES)}

# The kit is what the grid is: eight voices, eight pads.
PAD_COUNT = len(PAGES)

def bank(name, pid, slots, **extra):
    # ONE BANK IS ONE PAGE (the Tablor rule) and Movy's selectBankForPad
    # inherits it: a bank of more than 8 slots overflows onto a page nobody
    # named, which mis-targets pad-follow as well as shifting every following
    # page's label. Checked for EVERY bank, not just the voice pages.
    if len(slots) > 8:
        raise SystemExit(f"movy bank {name!r} has {len(slots)} slots -- max 8")
    b = {"name": name}
    if pid in PAD_OF_LEVEL:
        b["pad"] = PAD_OF_LEVEL[pid]
    b.update(extra)
    b["rows"] = [[movy_slot(p) for p in slots] + [None] * (8 - len(slots))]
    return b

# VOICES FIRST, then the pages that have no voice behind them.
#
# Movy takes exactly one shape here, and it is the same shape padSpecific has
# always had (forge, weird-dreams) -- a pad-following page first, ordinary
# pages behind it:
#
#     banks: [ voice, voice, ..., Master, Reverb, Delay ]
#              ^-- each declares `pad`     ^-- none of them do
#
# The voice pages share ONE seat in Movy's jog rotation, so this kit reads
# <voice> -> Master -> Reverb -> Delay: four pages, not eleven. The pad picks
# which voice that first seat holds, and only while a voice page is the one
# open -- pressing a pad on Reverb re-points the voice page without dragging
# you off Reverb. The run must LEAD: a `pad` on anything behind it is ignored,
# and a voice bank behind Master would stop being a voice.
#
# This differs from our own editor, which opens on Master (ui_chain.js sends
# "root" to page 0). Movy opens on the voice you last hit, the way it opens
# forge and weird-dreams, and Master is one jog click away.
#
# NOT "global": in Movy that flag means the params are not chain-addressable
# and therefore cannot be automated or LFO'd. Every one of ours is a real
# chain_param, so the flag was silently costing the whole master strip --
# dist, drive, volume, comp, velocity, choke, note map -- its automation.
# Spotted by Dima reviewing 8W8, which carried the same flag; 9W9's identical
# Main bank never did.
banks = []
for pid, label, params in PAGES:
    full = [PAGE_SWAP.get(p["key"], p) for p in params] + PAGE_SENDS[pid]
    banks.append(bank(MOVY_NAME[pid], pid, full))
for pid, label, params in FX_PAGES:
    banks.append(bank(label, pid, params))
# Master LAST. Reading the chain left to right -- voices, their sends, then the
# bus everything lands on -- is the order the rest of the Schwung fleet uses,
# and the one a chain view implies. Our own editor still opens on it.
banks.append(bank("Master", "root", GLOBALS))
movy = {"id": "6w6", "name": "6W6",
        # No padFollowLock: Movy is dropping the pad-follow lock from PR #16
        # (Shift + jog click is spoken for, and a gesture only three modules
        # answer to is worse than no gesture). The device keeps its own lock --
        # a plain jog click on Main, see mainLocked() in ui_chain.js -- which
        # is unaffected: that one is ours and runs in our editor.
        "drum": {"padCount": PAD_COUNT, "padNoteStart": 36, "rawMidi": False},
        "banks": banks}
# A pad past padCount resolves to nothing (drumPadOfPhys returns -1), which
# makes the page jog-only and silently un-followable. CW-78 shipped exactly
# that -- padCount 14 with Master on 15 -- so assert it rather than trust it.
for b in banks:
    if "pad" in b and not (1 <= b["pad"] <= PAD_COUNT):
        raise SystemExit(f"movy bank {b['name']!r} pad {b['pad']} outside 1..{PAD_COUNT}")
_pads = [b["pad"] for b in banks if "pad" in b]
if len(_pads) != len(set(_pads)):
    raise SystemExit(f"movy: two banks claim the same pad: {sorted(_pads)}")
# The voice run has to LEAD, with nothing claiming a pad behind it -- Movy
# reads the leading run as the voices and ignores a pad on anything after.
for _i, _b in enumerate(banks):
    if _i < PAD_COUNT and "pad" not in _b:
        raise SystemExit(f"movy bank {_i} {_b['name']!r} is inside the voice run "
                         "but declares no pad")
    if _i >= PAD_COUNT and "pad" in _b:
        raise SystemExit(f"movy bank {_b['name']!r} sits behind the voice run and "
                         f"claims pad {_b['pad']}; Movy ignores that and the page "
                         "becomes jog-only")
if sorted(_pads) != list(range(1, PAD_COUNT + 1)):
    raise SystemExit(f"the voice run must claim pads 1..{PAD_COUNT} exactly, got {sorted(_pads)}")
(root_dir / "src/movy_config.json").write_text(json.dumps(movy, indent=2) + "\n")

print(f"chain_params {len(cpj)}B  ui_pages {len(uhj)}B  movy banks={len(banks)}  "
      f"pages={len(levels)}  pots={len(pots)}  enums={len(enums)}  "
      f"params={len(cp)}")
