"""Backs up the slot contents of a Chameleon over USB (read only, restores the active slot).

    python3 backup_slots.py /dev/cu.usbmodemXXXX captures/backup/slots.json [slot_count]
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import chameleon_com  # noqa: E402
import chameleon_cmd  # noqa: E402
from chameleon_enum import SlotNumber, TagSenseType  # noqa: E402


def backup(port, count):
    com = chameleon_com.ChameleonCom()
    com.open(port)
    cmd = chameleon_cmd.ChameleonCMD(com)
    out = {"version": cmd.get_app_version(), "slots": []}
    active = cmd.get_active_slot()
    out["active"] = active
    info = cmd.get_slot_info()
    enabled = cmd.get_enabled_slots()
    try:
        for i in range(count):
            cmd.set_active_slot(SlotNumber.from_fw(i))
            slot = {"index": i, "info": info[i], "enabled": enabled[i]}
            if info[i]["hf"]:
                ac = cmd.hf14a_get_anti_coll_data()
                slot["anti_coll"] = {k: bytes(v).hex() for k, v in ac.items()}
                slot["blocks"] = "".join(
                    bytes(cmd.mf1_read_emu_block_data(k, 16)).hex() for k in range(0, 64, 16)
                )
                for name, fn in (("emulator_config", cmd.mf1_get_emulator_config),):
                    try:
                        v = fn()
                        slot[name] = v if not isinstance(v, (bytes, bytearray)) else bytes(v).hex()
                    except Exception as e:  # not every reference firmware has every call
                        slot[name] = f"unavailable: {e}"
            if info[i]["lf"]:
                try:
                    slot["lf_id"] = bytes(cmd.em410x_get_emu_id()).hex()
                except Exception as e:
                    slot["lf_id"] = f"unavailable: {e}"
            out["slots"].append(slot)
    finally:
        cmd.set_active_slot(SlotNumber.from_fw(active))
        com.close()
    return out


if __name__ == "__main__":
    data = backup(sys.argv[1], int(sys.argv[3]) if len(sys.argv) > 3 else 8)
    with open(sys.argv[2], "w") as f:
        json.dump(data, f, indent=1, default=str)
    print("saved", sys.argv[2], "slots", len(data["slots"]), "active", data["active"])
