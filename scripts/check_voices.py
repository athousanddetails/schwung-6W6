#!/usr/bin/env python3
"""Assert Schwung 0.13's voices contract, straight out of the generated header.

Nothing else checks this. The declaration is inert on a pre-0.13 host, so a
wrong note or a level id that resolves to nothing produces no error anywhere --
9W9 shipped `level_of[]` answering "hat","hat","cym","cym" against a hierarchy
with four separate pages, and four of its eleven voices silently never followed
the pad for months.

    ./scripts/check_voices.py
"""
import json, re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
HDR  = (ROOT / "src/dsp/sd606_params.h").read_text()
PLUG = (ROOT / "src/dsp/sd606_plugin.cpp").read_text()
ENG  = (ROOT / "src/dsp/sd606_engine.h").read_text()

fails = []
def check(cond, msg):
    print(("ok  : " if cond else "FAIL: ") + msg)
    if not cond: fails.append(msg)

def blob(name):
    i = HDR.index(name + "[] =")
    body = HDR[i:HDR.index(";", i)]
    return json.loads("".join(json.loads(l.strip())
                              for l in body.split("\n")[1:] if l.strip().startswith('"')))

drum, gm = blob("sd606_ui_pages_json"), blob("sd606_ui_pages_gm_json")

# The three orders the doc warns about, read from the SOURCE rather than assumed.
trigger = re.search(r"SD606_BD = 0,(.*?)SD606_NUM_VOICES", ENG, re.S).group(1)
trigger = ["bd"] + [m.lower() for m in re.findall(r"SD606_([A-Z]{2})\b", trigger)]
level_of = re.findall(r'"(\w+)"',
    re.search(r"kLevelOf\[SD606_NUM_VOICES\]\s*=\s*\{(.*?)\};", PLUG, re.S).group(1))
nav = [e["level"] for e in drum["levels"]["root"]["params"] if e.get("level")]
voices = [p for p in nav if "note" in drum["levels"][p]]

print(f"  trigger order : {trigger}")
print(f"  level_of[]    : {level_of}")
print(f"  nav order     : {nav}")

# --- the check that costs nothing to run and cost 9W9 four voices -------------
check(all(k in drum["levels"] for k in level_of),
      "every kLevelOf[] id is a level the generator emits")
check(level_of == trigger,
      "kLevelOf[] is in TRIGGER order, not nav order")

for name, m, want in (("drum-rack", drum, {"bd":36,"sd":37,"lt":38,"ht":39,
                                           "ch":40,"oh":41,"cy":42,"cp":43}),
                      ("GM",        gm,   {"bd":36,"sd":38,"lt":41,"ht":48,
                                           "ch":42,"oh":46,"cy":49,"cp":39})):
    check(m.get("pad_layout") == "drums",       f"{name}: pad_layout is \"drums\"")
    check(m.get("focus_param") == "ui_focus_level", f"{name}: focus_param declared")
    noted = {k: v["note"] for k, v in m["levels"].items() if "note" in v}
    check(len(noted) == 8, f"{name}: exactly 8 voices carry a note ({len(noted)})")
    check(noted == want,   f"{name}: notes match the plugin's router")
    check(len(set(noted.values())) == len(noted), f"{name}: no duplicate notes")
    silent = [k for k in ("rev", "dly", "root") if "note" in m["levels"].get(k, {})]
    check(not silent, f"{name}: no note on a level that makes no sound ({silent})")

# GM must be REAL GM, not merely internally consistent.
gmn = {k: v["note"] for k, v in gm["levels"].items() if "note" in v}
for voice, note, what in (("bd",36,"kick"), ("sd",38,"snare"), ("ch",42,"closed hat"),
                          ("oh",46,"open hat"), ("cy",49,"crash"), ("cp",39,"hand clap")):
    check(gmn.get(voice) == note, f"GM {voice} is the real GM {what} ({note})")

# Both maps must describe the SAME instrument.
check(set(drum["levels"]) == set(gm["levels"]), "both maps declare the same levels")
check({k: v.get("role") for k, v in drum["levels"].items()}
      == {k: v.get("role") for k, v in gm["levels"].items()}, "roles agree across maps")

# The declaration has to match what the router actually does.
for note, voice in re.findall(r"case (\d+):\s*return SD606_([A-Z]{2});", PLUG):
    v = voice.lower()
    if gmn.get(v) is not None and int(note) == gmn[v]:
        break
else:
    check(False, "each declared GM note appears in the plugin's GM switch")
missing = [v for v, n in gmn.items()
           if f"case {n}:" not in PLUG.split("GM drum map")[1].split("default")[0]]
check(not missing, f"every declared GM note is routed by the plugin ({missing})")

print(f"\n{'FAILED (' + str(len(fails)) + ')' if fails else 'ALL PASS'}")
sys.exit(1 if fails else 0)
