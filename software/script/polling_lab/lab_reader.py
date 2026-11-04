"""Reader profiles for the polling lab. Runs inside the Proxmark3 client:

    proxmark3 -p /dev/cu.usbmodemiceman1 -y lab_reader.py -- <profile> <out.jsonl> [key=value ...]

Every PM3 command, its host side timing and its parsed result is written as one JSON line.
Command strings live in CMD below so they can be adjusted after a first look at the real
PM3 output (nothing here has run against hardware yet).
"""
import json
import re
import sys
import time

ANSI = re.compile(r"\x1b\[[0-9;]*m")

CMD = {
    "field_on_reqa": "hf 14a raw -a -k -b 7 26",   # field on + REQA (clears the PM3 trace)
    "reqa": "hf 14a raw -k -b 7 26",               # REQA, field stays on, trace keeps growing
    "wupa": "hf 14a raw -k -b 7 52",               # WUPA, also wakes a HALTed card
    "anticol1": "hf 14a raw -k 9320",              # CL1 anticollision
    "select1": "hf 14a raw -kc 9370{uid5}",        # CL1 select, CRC appended
    "halt": "hf 14a raw -kcr 5000",                # HALT, no response expected
    "auth": "hf 14a raw --crypto1 -sk3c 60{blk}{key}",     # select (no RATS: SAK 28/20 slots) + auth in one PM3 command
    "read": "hf 14a raw --crypto1 -kc 30{blk}",
    "drop": "hf 14a reader --drop",
    "trace": "trace list -t 14a -u --frame",
    "trace_save": "trace save -f {path}",
}


class RealDriver:
    def __init__(self):
        import pm3  # only available inside the PM3 client
        self.p = pm3.pm3()

    def run(self, cmd):
        self.p.console(cmd, capture=True, quiet=True)
        return ANSI.sub("", self.p.grabbed_output or "")


def hex_bytes(out):
    """Bytes of the last all-hex line of a PM3 reply, or None. PM3 marks CRC bytes with [ ]."""
    found = None
    for line in out.splitlines():
        line = re.sub(r"^\s*\[[+=!]\]\s*", "", line).replace("[", " ").replace("]", " ").strip()
        if re.fullmatch(r"(?:[0-9A-Fa-f]{2}\s*)+", line):
            found = bytes.fromhex(re.sub(r"\s+", "", line))
    return found


def auth_ok(out):
    """PM3 answers a successful crypto1 auth shortcut with the 4 bit ACK 0x0A; a failure
    shows up as another value (e.g. 04) or an error text."""
    if looks_failed(out):
        return False
    return hex_bytes(out) == b"\x0a"


def read_ok(out):
    data = hex_bytes(out)
    return data is not None and len(data) >= 16


def looks_failed(out):
    return bool(re.search(r"fail|error|denied|no response|timeout|timed out|wrong", out, re.I))


class Session:
    def __init__(self, driver, out_path, params):
        self.d = driver
        self.f = open(out_path, "w")
        self.p = params
        self.t0 = time.perf_counter()
        self.field = False
        self.last_poll_silent = False

    def now(self):
        return time.perf_counter() - self.t0

    def log(self, **kw):
        kw["t"] = round(self.now(), 6)
        self.f.write(json.dumps(kw) + "\n")
        self.f.flush()

    def run(self, name, **fmt):
        cmd = CMD[name].format(**fmt)
        t_start = self.now()
        out = self.d.run(cmd)
        t_end = self.now()
        if re.search(r"field is off|can't select|no tag found", out, re.I):
            self.field = False  # PM3 dropped the field itself, the next poll must power it up again
        self.log(ev="cmd", cmd=cmd, t_start=round(t_start, 6), dt_ms=round((t_end - t_start) * 1000, 2),
                 out=out.strip()[-300:])
        return out

    # ---- reader building blocks -------------------------------------------------
    def field_on(self):
        out = self.run("field_on_reqa")
        self.field = True
        return hex_bytes(out)

    def field_reset(self, off_ms):
        self.run("drop")
        self.field = False
        time.sleep(off_ms / 1000.0)

    def poll_uid(self, select=True):
        """REQA + anticollision (+ select) for 4 byte UIDs. Returns uid bytes or None.
        select=False leaves the card in READY state, the following `auth -s` selects on its own."""
        mode = self.p.get("poll_cmd", "reqa")  # reqa | wupa | alt (WUPA after an unanswered poll)
        if not self.field:
            atqa = self.field_on()
        else:
            use_wupa = mode == "wupa" or (mode == "alt" and self.last_poll_silent)
            atqa = hex_bytes(self.run("wupa" if use_wupa else "reqa"))
        self.last_poll_silent = not atqa
        if not atqa:
            return None
        ac = hex_bytes(self.run("anticol1"))
        if not ac or len(ac) < 5:
            return None
        if select:
            self.run("select1", uid5=ac[:5].hex())
        return ac[:4]

    def classic_attempt(self, key, blk):
        """UID + one auth attempt (the PM3 re-selects for the auth). Returns (uid, ok, read_ok)."""
        uid = self.poll_uid(select=False)
        if uid is None:
            return None, None, None
        out = self.run("auth", blk="%02x" % blk, key=key)
        ok = auth_ok(out)
        read = None
        if ok:
            read = read_ok(self.run("read", blk="%02x" % blk))
        return uid, ok, read


# ---- profiles ----------------------------------------------------------------------
def profile_uid_only(s, accept_uid=None, whitelist=False):
    uid = s.poll_uid()
    if uid is None:
        return False
    s.run("halt")
    s.log(ev="result", uid=uid.hex(), auth=None)
    # plain uid_only readers just keep reading; a whitelist reader opens for one UID only
    return whitelist and accept_uid is not None and uid.hex() == accept_uid.lower()


def profile_classic(s, key, blk, accept_uid=None):
    uid, ok, read_ok = s.classic_attempt(key, blk)
    if uid is None:
        return False
    s.log(ev="result", uid=uid.hex(), auth="ok" if ok else "fail", read=read_ok)
    return bool(ok and read_ok and (accept_uid is None or uid.hex() == accept_uid.lower()))


# Android probes the MAD and NFC Forum keys first (NDEF discovery), the default key comes later
PHONE_KEYS = ["A0A1A2A3A4A5", "D3F7D3F7D3F7", "FFFFFFFFFFFF", "000000000000"]


def profile_phone(s, blk):
    """Probes the dictionary on one sector, then falls back to presence checks."""
    uid = s.poll_uid()
    if uid is None:
        return False
    for key in PHONE_KEYS:
        out = s.run("auth", blk="%02x" % blk, key=key)
        ok = auth_ok(out)
        s.log(ev="result", uid=uid.hex(), auth="ok" if ok else "fail", key=key)
        if ok:
            s.run("read", blk="%02x" % blk)
            return True
    time.sleep(0.2)
    s.run("reqa")
    return False


def main(argv):
    if len(argv) < 2:
        raise SystemExit("usage: -y lab_reader.py -- <profile> <out.jsonl> [key=value ...]")
    profile, out_path = argv[0], argv[1]
    params = dict(a.split("=", 1) for a in argv[2:])
    period = float(params.get("period_ms", "30")) / 1000.0
    duration = float(params.get("duration_s", "8"))
    rf_reset = float(params.get("rf_reset_ms", "0"))
    hold = float(params.get("hold_ms", "1500")) / 1000.0
    key = params.get("key", "FFFFFFFFFFFF")
    blk = int(params.get("blk", "0"))
    accept_uid = params.get("accept_uid")

    post = float(params.get("post_s", "0"))  # keep reading this long after the accept, field stays on
    s = Session(RealDriver(), out_path, params)
    s.log(ev="start", profile=profile, params=params)
    accepted = False
    t_accept = None
    try:
        run_loop(s, profile, params, period, duration, rf_reset, hold, key, blk, accept_uid, post)
    finally:
        s.run("drop")
        # the full annotated trace is the primary evidence, keep it untruncated
        text = s.d.run(CMD["trace"])
        with open(out_path + ".trace.txt", "w") as tf:
            tf.write(text)
        s.d.run(CMD["trace_save"].format(path=out_path + ".trace"))
        s.log(ev="end")
        s.f.close()


def run_loop(s, profile, params, period, duration, rf_reset, hold, key, blk, accept_uid, post):
    accepted = False
    t_accept = None
    while s.now() < duration:
        if accepted and (post <= 0 or s.now() - t_accept >= post):
            break
        cycle_start = s.now()
        was_accepted = accepted
        if profile == "uid_only":
            accepted = profile_uid_only(s)
        elif profile == "uid_whitelist":
            accepted = profile_uid_only(s, accept_uid=accept_uid, whitelist=True)
        elif profile in ("classic", "wrongkey"):
            accepted = profile_classic(s, key, blk, accept_uid)
        elif profile == "phone":
            accepted = profile_phone(s, blk)
        else:
            raise SystemExit("unknown profile " + profile)
        if accepted and not was_accepted:
            t_accept = s.now()
            s.log(ev="accept")
            if post <= 0:
                if hold > 0:
                    s.field_reset(hold * 1000)  # "door opens": the field goes away for a while
                break
        if rf_reset > 0 and not accepted:
            s.field_reset(rf_reset)
        rest = period - (s.now() - cycle_start)
        if rest > 0:
            time.sleep(rest)


if __name__ == "__main__":
    main(sys.argv[1:])
