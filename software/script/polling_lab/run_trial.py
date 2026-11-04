"""One lab trial: configure the Chameleon, run a PM3 reader profile, collect both logs.

    python3 run_trial.py <chameleon_port> <pm3_port> <engine> <start_slot0> <out_prefix> <profile> [key=value ...]

Needs the Chameleon on USB (the lab flag lets it poll without a sleep/wake cycle).
"""
import json
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lab_chameleon as lc  # noqa: E402
import analyze  # noqa: E402
from chameleon_enum import Command  # noqa: E402


def run(cham_port, pm3_port, engine, start_slot, prefix, profile, params, mask=0x0F, cfg=None):
    time.sleep(1.5)  # let the previous trial's field loss settle
    com = lc.connect(cham_port)
    try:
        # hold the slot while no field is there, so every trial starts from the same state
        lc.set_lab(com, lc.LAB_ANY_FIELD | lc.LAB_HOLD_IDLE, engine, clear=True)
        # a manual slot switch stops polling until the next config, so switch first
        lc.send(com, Command.SET_ACTIVE_SLOT, struct.pack("!B", start_slot))
        lc.set_polling(com, True, mask, **(cfg or {}))
        time.sleep(0.5)
        lc.set_lab(com, lc.LAB_ANY_FIELD | lc.LAB_LOG | lc.LAB_HOLD_IDLE, engine, clear=True)
    finally:
        com.close()
    time.sleep(1.0)
    reader_log = prefix + ".reader.jsonl"
    args = [a for a in params]
    cmd = ["proxmark3", pm3_port, "-y", os.path.join(os.path.dirname(os.path.abspath(__file__)), "lab_reader.py"),
           "--", profile, reader_log] + args
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    com = lc.connect(cham_port)
    try:
        entries = lc.read_log(com)
        lc.set_lab(com, 0, 0, clear=False)
    finally:
        com.close()
    with open(prefix + ".cham.json", "w") as f:
        json.dump(entries, f)
    res = analyze.analyze(analyze.load_jsonl(reader_log))
    res["chameleon"] = analyze.chameleon_summary(entries)
    return res, entries


if __name__ == "__main__":
    res, entries = run(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], sys.argv[6], sys.argv[7:])
    res["visits"] = [(v["slot"], v["n"], v.get("dwell_ms")) for v in res["visits"]]
    print(json.dumps(res))
