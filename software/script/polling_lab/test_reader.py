"""Offline test of lab_reader's poll logic against a fake PM3 + a card that ignores REQA."""
import json, os, sys, tempfile
sys.argv = ["x"]
import lab_reader as lr


class FakeDriver:
    """A card that answers WUPA but not REQA until it has been woken once."""
    def __init__(self):
        self.woken = False
        self.log = []

    def run(self, cmd):
        self.log.append(cmd)
        if cmd.startswith("hf 14a raw -a -k -b 7 26"):
            return ""
        if cmd.startswith("hf 14a raw -k -b 7 52"):
            self.woken = True
            return "[+] 04 00"
        if cmd.startswith("hf 14a raw -k -b 7 26"):
            return "[+] 04 00" if self.woken else ""
        if cmd.startswith("hf 14a raw -k 9320"):
            return "[+] 01 02 03 [ 04 04 ]"
        if "--crypto1 -skc" in cmd:
            return "[+] 04"
        return ""


def test_alt_uses_wupa_after_silence():
    d = FakeDriver()
    s = lr.Session(d, os.path.join(tempfile.mkdtemp(), "o.jsonl"), {"poll_cmd": "alt"})
    assert s.poll_uid(select=False) is None   # field_on: silent
    uid = s.poll_uid(select=False)            # silence last time -> WUPA, wakes the card
    assert uid == bytes.fromhex("01020304"), uid
    assert any("52" in c and "-b 7" in c for c in d.log)


def test_reqa_only_never_wakes():
    d = FakeDriver()
    s = lr.Session(d, os.path.join(tempfile.mkdtemp(), "o.jsonl"), {})
    for _ in range(4):
        assert s.poll_uid(select=False) is None
    assert not d.woken


if __name__ == "__main__":
    test_alt_uses_wupa_after_silence()
    test_reqa_only_never_wakes()
    print("ok")
