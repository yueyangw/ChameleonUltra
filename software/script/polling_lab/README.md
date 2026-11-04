# polling_lab

PM3 acts as a reader, the Chameleon polls its slots. Everything here is a development tool.

**The Chameleon must not be exposed to the PM3 field unless you know what that costs.**
A reference firmware with a trial limit counts every field-woken polling session.

- `lab_reader.py`  runs inside the PM3 client: `proxmark3 -p <port> -y lab_reader.py -- <profile> <out.jsonl> [key=value ...]`
- `lab_chameleon.py` host side control of our firmware (slots, polling config, lab flags, event log)
- `analyze.py` turns a `lab_reader` log (+ optional Chameleon event log) into metrics
- `test_analyze.py` offline self test of the analyzer (`python3 test_analyze.py`)

Profiles: `uid_only`, `uid_whitelist`, `classic`, `wrongkey`, `phone`.
Common parameters: `period_ms`, `duration_s`, `rf_reset_ms` (0: field stays on), `key`, `blk`, `accept_uid`, `hold_ms`.
