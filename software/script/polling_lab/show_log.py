"""Compact timeline of a Chameleon event log (+ optional reader log), times in ms."""
import json, sys

DEC = {1: "AUTH_FAIL", 2: "READS_DONE", 3: "IGNORED", 4: "LOCK_IDLE", 5: "STALLED", 6: "RF_RESET", 7: "UNHALT"}
rows = []
for e in json.load(open(sys.argv[1])):
    arg = DEC.get(e["arg"], e["arg"]) if e["event"] == "DECIDE" else (e["arg"] if e["arg"] else "")
    rows.append((e["t"], f'{e["event"]:10} slot={e["slot"]} {arg}'))
if len(sys.argv) > 2:
    # reader events are relative to the PM3 script start, Chameleon ones to FIELD_ON: align on the first reader command
    t_field = next((t for t, s in rows if s.startswith("FIELD_ON")), 0.0)
    for l in open(sys.argv[2]):
        e = json.loads(l)
        if e["ev"] == "result":
            rows.append((t_field + e["t"] - 0.23, "   READER " + f'uid={e["uid"]} auth={e.get("auth")}'))
rows.sort()
last = None
for t, s in rows:
    print(f"{t*1000:8.1f}  {s}")
