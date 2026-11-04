"""Runs a set of reader scenarios against the Chameleon and prints a summary table.

    python3 run_matrix.py <chameleon_port> <pm3_port> <out_dir> [engine ...] [--repeat N] [--only name,name]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_trial  # noqa: E402

# Lab cards are site specific: pass the door key and the UIDs of slots 2 and 4 via the environment.
DOOR_KEY = os.environ.get("LAB_DOOR_KEY", "FFFFFFFFFFFF")
UID_SLOT2 = os.environ.get("LAB_UID_SLOT2", "11223344")
UID_SLOT4 = os.environ.get("LAB_UID_SLOT4", "55667788")
SCENARIOS = {
    # name: (profile, params, start_slot, target_slot or None)
    "door_from_slot1": ("classic", [f"key={DOOR_KEY}", "blk=8"], 0, 2),
    "door_from_slot2": ("classic", [f"key={DOOR_KEY}", "blk=8"], 1, 2),
    "door_from_slot4": ("classic", [f"key={DOOR_KEY}", "blk=8"], 3, 2),
    "door_already":    ("classic", [f"key={DOOR_KEY}", "blk=8"], 2, 2),
    "door_rf_reset":   ("classic", [f"key={DOOR_KEY}", "blk=8", "rf_reset_ms=30"], 0, 2),
    "uid_last_slot":   ("uid_whitelist", [f"accept_uid={UID_SLOT4}"], 0, 3),
    "uid_second_slot": ("uid_whitelist", [f"accept_uid={UID_SLOT2}"], 3, 1),
    "wrong_key":       ("wrongkey", ["key=A0A1A2A3A4A5", "blk=8"], 0, None),
    "phone":           ("phone", ["blk=0"], 3, None),
    "phone_from_slot1": ("phone", ["blk=0"], 0, None),
}


def hit_time_ms(entries, target):
    """Chameleon side: first reader poll -> auth ok / select on the target slot."""
    t0 = next((e["t"] for e in entries if e["event"] == "POLL"), None)
    if t0 is None or target is None:
        return None
    for e in entries:
        if e["t"] >= t0 and e["slot"] == target and e["event"] in ("AUTH_OK", "SELECT"):
            if e["event"] == "AUTH_OK" or True:
                return round((e["t"] - t0) * 1000, 0)
    return None


def main():
    args = sys.argv[1:]
    cham, pm3, outdir = args[0], args[1], args[2]
    rest = args[3:]
    repeat = 1
    only = None
    cfg = {}
    engines = []
    i = 0
    while i < len(rest):
        if rest[i] == "--repeat":
            repeat = int(rest[i + 1]); i += 2
        elif rest[i] == "--only":
            only = rest[i + 1].split(","); i += 2
        elif rest[i] == "--cfg":
            cfg = {k: int(v) for k, v in (kv.split("=") for kv in rest[i + 1].split(","))}; i += 2
        else:
            engines.append(int(rest[i])); i += 1
    engines = engines or [1]
    os.makedirs(outdir, exist_ok=True)
    print(f'{"scenario":18} {"eng":3} {"ok":3} {"reader ms":>9} {"cham ms":>8} {"switches":>8} {"last slot":>9}')
    for name, (profile, params, start, target) in SCENARIOS.items():
        if only and name not in only:
            continue
        for engine in engines:
            for r in range(repeat):
                prefix = os.path.join(outdir, f"{name}_e{engine}_{r}")
                p = ["duration_s=5", "period_ms=100", "post_s=0.3", "hold_ms=0", "poll_cmd=alt"] + params
                res, entries = run_trial.run(cham, pm3, engine, start, prefix, profile, p, cfg=cfg)
                last = next((e["slot"] for e in reversed(entries) if e["event"] in ("SELECT", "AUTH_OK", "AUTH_FAIL")), None)
                print(f'{name:18} {engine:3} {"yes" if res["accepted"] else "no":3} '
                      f'{res["time_to_accept_ms"] if res["accepted"] else "-":>9} {hit_time_ms(entries, target) or "-":>8} '
                      f'{res["chameleon"]["switch_count"]:>8} {last if last is not None else "-":>9}', flush=True)
                time.sleep(1.0)


if __name__ == "__main__":
    main()
