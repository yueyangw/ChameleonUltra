"""Host side control of the Chameleon running the polling lab firmware.

Usage examples:
    python3 lab_chameleon.py /dev/cu.usbmodemXXXX setup 4
    python3 lab_chameleon.py /dev/cu.usbmodemXXXX lab 3 0        # any-field trigger + log, engine 0
    python3 lab_chameleon.py /dev/cu.usbmodemXXXX dump out.json
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import chameleon_com  # noqa: E402
from chameleon_enum import Command, TagSpecificType  # noqa: E402

CMD_POLLING_CONFIG_GET = 1902
CMD_POLLING_CONFIG_SET = 1903
CMD_POLLING_STATUS = 1904
CMD_LAB_SET = 1905
CMD_LAB_LOG = 1906

LAB_ANY_FIELD = 0x01
LAB_LOG = 0x02
LAB_HOLD_IDLE = 0x04

TICK_HZ = 16384  # APP_TIMER_CONFIG_RTC_FREQUENCY 1 -> prescaler 1
TICK_MASK = 0xFFFFFF

EVENT_NAMES = {
    1: "FIELD_ON", 2: "FIELD_OFF", 3: "POLL", 4: "SELECT", 5: "AUTH_OK",
    6: "AUTH_FAIL", 7: "HALT", 8: "SWITCH", 9: "GAP_BEGIN", 10: "GAP_END",
    11: "START", 12: "STOP", 13: "DECIDE", 14: "PROF",
}


def slot_uid(slot: int) -> bytes:
    """Distinct 4 byte UID per slot, so the reader side can tell slots apart."""
    return bytes([0xA0 + slot, 0x10 + slot, 0x20 + slot, 0x30 + slot])


def slot_key(slot: int) -> bytes:
    return bytes([0xC0 + slot, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5])


def connect(port: str) -> chameleon_com.ChameleonCom:
    com = chameleon_com.ChameleonCom()
    com.open(port)
    return com


def send(com, cmd: int, data: bytes = b"", timeout: int = 5):
    resp = com.send_cmd_sync(cmd, data if data else None, timeout=timeout)
    if resp.status != 0x68 and resp.status != 0x00:  # SUCCESS variants
        pass
    return resp


def setup_slots(com, count: int, key_slots=None):
    """Slots 0..count-1: MIFARE 1K, unique UID, unique key A on sector 0, HF enabled."""
    for i in range(count):
        send(com, Command.SET_ACTIVE_SLOT, struct.pack("!B", i))
        send(com, Command.SET_SLOT_DATA_DEFAULT, struct.pack("!BH", i, TagSpecificType.MIFARE_1024))
        uid = slot_uid(i)
        data = struct.pack("!B4s2s1sB", 4, uid, b"\x04\x00", b"\x08", 0)
        send(com, Command.HF14A_SET_ANTI_COLL_DATA, data)
        # block 0 carries the UID, block 3 the sector trailer
        bcc = uid[0] ^ uid[1] ^ uid[2] ^ uid[3]
        block0 = uid + bytes([bcc, 0x08, 0x04]) + bytes(9)
        trailer = slot_key(i) + bytes.fromhex("FF078069") + bytes.fromhex("FFFFFFFFFFFF")
        send(com, Command.MF1_WRITE_EMU_BLOCK_DATA, struct.pack("!B", 0) + block0)
        send(com, Command.MF1_WRITE_EMU_BLOCK_DATA, struct.pack("!B", 3) + trailer)
        send(com, Command.SET_SLOT_ENABLE, struct.pack("!BBB", i, 2, 1))
    send(com, Command.SLOT_DATA_CONFIG_SAVE)
    send(com, Command.SET_ACTIVE_SLOT, struct.pack("!B", 0))


def set_polling(com, enabled=True, slot_mask=0, represent=1, fail_count=3, done_quiet=100, patience=800,
                sticky=3, fail_poll_quiet=150, lock_quiet_100ms=10):
    """Writes a v7 polling record (24 bytes); the idle/active dwell and cooldown stay at their defaults."""
    rec = struct.pack(
        "<BBHHHHBBBBHHBHBBB",
        7, 1 if enabled else 0, slot_mask, 500, 2000, 200, 1, 10, 0, 1,
        done_quiet, patience, sticky, fail_poll_quiet, represent, fail_count, lock_quiet_100ms,
    )
    assert len(rec) == 24, len(rec)
    return send(com, CMD_POLLING_CONFIG_SET, rec)


def set_lab(com, flags: int, engine: int, clear: bool = True):
    resp = send(com, CMD_LAB_SET, struct.pack("BBBB", flags, engine, 1 if clear else 0, 0))
    return struct.unpack("<BBxxI", resp.data)


def read_log(com):
    entries = []
    index = 0
    while True:
        resp = send(com, CMD_LAB_LOG, struct.pack("<IB", index, 64))
        total = struct.unpack_from("<I", resp.data, 0)[0]
        if index < total - 1024:
            index = total - 1024  # the ring only holds the newest 1024 entries
            continue
        body = resp.data[4:]
        n = len(body) // 8
        for k in range(n):
            tick, ev, slot, arg = struct.unpack_from("<IBBH", body, k * 8)
            entries.append({"tick": tick, "event": EVENT_NAMES.get(ev, str(ev)), "slot": slot, "arg": arg})
        index += n
        if n == 0 or index >= total:
            break
    # unwrap the 24 bit tick counter into seconds relative to the first entry
    if entries:
        base = entries[0]["tick"]
        last = base
        wrap = 0
        for e in entries:
            if e["tick"] < last and last - e["tick"] > TICK_MASK // 2:
                wrap += TICK_MASK + 1
            last = e["tick"]
            e["t"] = (e["tick"] + wrap - base) / TICK_HZ
    return entries


def main(argv):
    port, action = argv[1], argv[2]
    com = connect(port)
    try:
        if action == "setup":
            setup_slots(com, int(argv[3]))
            set_polling(com, True, (1 << int(argv[3])) - 1)
        elif action == "lab":
            print(set_lab(com, int(argv[3]), int(argv[4])))
        elif action == "dump":
            with open(argv[3], "w") as f:
                json.dump(read_log(com), f)
        else:
            raise SystemExit("unknown action")
    finally:
        com.close()


if __name__ == "__main__":
    main(sys.argv)
