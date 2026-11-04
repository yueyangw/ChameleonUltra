"""Turns a lab_reader log (+ optional Chameleon event log) into metrics.

    python3 -I analyze.py reader.jsonl [chameleon_log.json]

Reader log events used: start, cmd(t_start, dt_ms), result(uid, auth), accept, end.
Slot of a UID: a0..af first byte = 0xA0 + slot (see lab_chameleon.slot_uid); other UIDs
are reported as hex.
"""
import json
import sys


def load_jsonl(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def uid_slot(uid_hex):
    if uid_hex and len(uid_hex) == 8 and uid_hex[0] == "a":
        return int(uid_hex[1], 16)
    return uid_hex


def analyze(events):
    results = [e for e in events if e["ev"] == "result" and e.get("uid")]
    accept = next((e for e in events if e["ev"] == "accept"), None)
    end = next((e for e in events if e["ev"] == "end"), None)
    first_cmd = next((e for e in events if e["ev"] == "cmd"), None)
    t0 = first_cmd["t_start"] if first_cmd else 0.0

    # visits: consecutive results with the same UID form one visit to a slot
    visits = []
    for r in results:
        slot = uid_slot(r["uid"])
        if visits and visits[-1]["slot"] == slot:
            visits[-1]["end"] = r["t"]
            visits[-1]["n"] += 1
            if r.get("auth"):
                visits[-1]["auth"].append(r["auth"])
        else:
            visits.append({"slot": slot, "start": r["t"], "end": r["t"], "n": 1,
                           "auth": [r["auth"]] if r.get("auth") else []})

    for v in visits:
        v["dwell_ms"] = round((v["end"] - v["start"]) * 1000, 1)

    return {
        "accepted": accept is not None,
        "time_to_accept_ms": round((accept["t"] - t0) * 1000, 1) if accept else None,
        "switches": max(len(visits) - 1, 0),
        "cycles": len(results),
        "visits": visits,
        "duration_s": end["t"] if end else None,
    }


def chameleon_summary(entries):
    """Event-log based numbers: switch latency after an auth failure, polls per visit."""
    out = {"switch_after_auth_fail_ms": [], "switch_count": 0}
    last_fail = None
    for e in entries:
        if e["event"] == "AUTH_FAIL":
            last_fail = e["t"]
        elif e["event"] == "SWITCH":
            out["switch_count"] += 1
            if last_fail is not None:
                out["switch_after_auth_fail_ms"].append(round((e["t"] - last_fail) * 1000, 1))
                last_fail = None
    return out


def main(argv):
    res = analyze(load_jsonl(argv[1]))
    if len(argv) > 2:
        with open(argv[2]) as f:
            res["chameleon"] = chameleon_summary(json.load(f))
    print(json.dumps(res, indent=2))


if __name__ == "__main__":
    main(sys.argv)
