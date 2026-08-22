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
DIST = ["Diode", "Hard Clip", "Wavefolder", "Bitcrush"]
def DRIVE(v):  return P(f"{v}_drive", "Drive", 0.2, 8.0, EXP, 55)   # pot 55 == 1.0
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
        P("bd_drift",   "Drift",    0.0,  1.0,  LIN, 0),    # per-hit pitch variation
        DRIVE("bd"), DTYPE("bd"), LEVEL("bd"),
    ]),
    ("sd", "Snare", [
        # SnareVoice::trigger(decay, bodyPitchRatio, snappy, noiseColorRatio)
        # Defaults FITTED against the hardware snare (1680 renders, score
        # 11.3 -> 8.4): a touch sharper, shorter, less wire, darker wire.
        P("sd_tune",    "Tune",     0.5,  2.0,  EXP, 70),
        DECAY("sd", 76),
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
        E("hh_choke", "Choke", ["Off", "CH > OH", "Mutual"], 1),
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

GLOBALS = [
    E("master_dist", "Master Dist", ["Off"] + DIST),
    P("master_drive", "Master Drive", 0.2, 8.0, EXP, 55),
    # 0.63 at centre-ish, not 0.8: a dense accented pattern peaks around
    # -1.1 dBFS from here, so the kit leaves real headroom for whatever
    # the chain puts after it instead of arriving already clipped.
    P("volume", "Volume", 0.0, 1.0, LIN, 76),
    P("accent", "Accent", 1.0, 4.0, LIN, 42),               # 2.0x on accented hits
    E("hh_choke", "Choke", ["Off", "CH > OH", "Mutual"], 1),
    E("note_map", "Note Map", ["Drum Rack (36+)", "General MIDI"]),
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

def chain_param(p):
    if p["kind"] == "enum":
        d = {"key": p["key"], "name": p["name"], "type": "enum",
             "options": p["options"]}
    else:
        d = {"key": p["key"], "name": p["name"], "type": "int",
             "min": 0, "max": 127}
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

for pid, label, params in PAGES:
    for p in params:
        register(p)
    if len(params) > 8:
        raise SystemExit(f"page {pid} has {len(params)} params — max 8 knobs")
    levels[pid] = {"name": label,
                   "knobs": [p["key"] for p in params],
                   "params": [{"key": p["key"], "name": p["name"]} for p in params]}
    root.append({"level": pid, "label": label})

for p in GLOBALS:
    register(p)
root += [{"key": p["key"], "name": p["name"]} for p in GLOBALS]
levels["root"] = {"name": "6W6",
                  "knobs": [p["key"] for p in GLOBALS[:4]],
                  "params": root}

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
SHORT = {"Tune": "TUNE", "Decay": "DECAY", "Attack": "ATTK", "Drift": "DRIFT",
         "Drive": "DRIVE", "Distortion": "DIST", "Level": "LEVEL",
         "Snappy": "SNAPY", "Tone": "TONE", "Noise": "NOISE", "Choke": "CHOKE",
         "Master Dist": "MDIST", "Master Drive": "MDRV", "Volume": "VOL",
         "Accent": "ACNT", "Note Map": "NMAP"}
MOVY_NAME = {"bd": "Kick", "sd": "Snare", "lt": "Lo Tom", "ht": "Hi Tom",
             "ch": "Cl Hat", "oh": "Op Hat", "cy": "Cymbal", "cp": "Clap"}
def movy_slot(p):
    d = {"key": p["key"], "short": SHORT[p["name"]], "full": p["name"]}
    if p["kind"] == "enum":
        d["type"] = "enum"
        # Movy squeezes option text into a 32 px cell: short words only.
        d["options"] = [o.replace("Hard Clip", "Clip").replace("Wavefolder", "Fold")
                         .replace("Bitcrush", "Crush").replace("Drum Rack (36+)", "Rack")
                         .replace("General MIDI", "GM") for o in p["options"]]
    else:
        d["type"] = "int"; d["min"] = 0; d["max"] = 127
    return d
banks = []
for pid, label, params in PAGES:
    row = [movy_slot(p) for p in params] + [None] * (8 - len(params))
    banks.append({"name": MOVY_NAME[pid], "rows": [row]})
banks.append({"name": "Master", "global": True,
              "rows": [[movy_slot(p) for p in GLOBALS] + [None] * (8 - len(GLOBALS))]})
movy = {"id": "6w6", "name": "6W6",
        "drum": {"padCount": 16, "padNoteStart": 36, "rawMidi": False},
        "banks": banks}
(root_dir / "src/movy_config.json").write_text(json.dumps(movy, indent=2) + "\n")

print(f"chain_params {len(cpj)}B  ui_pages {len(uhj)}B  movy banks={len(banks)}  "
      f"pages={len(levels)}  pots={len(pots)}  enums={len(enums)}  "
      f"params={len(cp)}")
