#!/usr/bin/env python3
"""Print the generated page hierarchy and chain_params from sd606_params.h.

The payloads are C string literals split across lines; reassembling them by
hand got fiddly enough to be worth a tool, and it doubles as a check that both
JSON blobs actually parse."""
import json, pathlib, re, sys

src = pathlib.Path("src/dsp/sd606_params.h").read_text()

def cstr(name):
    i = src.index(name + "[] =")
    body = src[i:src.index(";", i)]
    parts = re.findall(r'^\s*"(.*)"\s*$', body, re.M)
    return json.loads("".join(json.loads('"' + p + '"') for p in parts))

pages = cstr("sd606_ui_pages_json")["levels"]
cp = {p["key"]: p for p in cstr("sd606_chain_params_json")}
order = [k for k in pages if k != "root"] + ["root"]
for k in order:
    v = pages[k]
    missing = [x for x in v["knobs"] if x not in cp]
    flag = "  !! NOT IN chain_params: " + ",".join(missing) if missing else ""
    print(f"  {k:5s} {len(v['knobs'])} knobs  {' '.join(x.split('_',1)[-1] for x in v['knobs'])}{flag}")
    if len(v["knobs"]) > 8:
        sys.exit(f"page {k} has more than 8 knobs")
    if missing:
        sys.exit("a page knob missing from chain_params turns into a 0-or-max toggle")
print(f"  {len(cp)} chain_params entries, {len(pages)} pages — all knobs typed")
