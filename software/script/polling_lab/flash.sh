#!/bin/bash
# Build, package and flash our firmware over USB (our firmware honours the DFU command).
# usage: flash.sh <scratch dir with clivenv/ and nrfvenv/>
#   clivenv: python venv with the CLI requirements, nrfvenv: nrfutil 6.1.7 (see README)
set -e
S="$1"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
(cd "$ROOT/firmware/application" && make -j8 GNU_INSTALL_ROOT=/opt/homebrew/bin/ 2>&1 | grep -E "warning|error" || true)
cd "$ROOT/firmware/objects"
rm -f ultra-dfu-app.zip
"$S/nrfvenv/bin/nrfutil" pkg generate --hw-version 0 --key-file ../../resource/dfu_key/chameleon.pem \
  --application application.hex --application-version 1 --sd-req 0x0100 ultra-dfu-app.zip >/dev/null

find_port() {
  "$S/clivenv/bin/python" - "$1" <<'PY'
import sys
import serial.tools.list_ports as lp
vid = int(sys.argv[1], 16)
for p in lp.comports():
    if p.vid == vid:
        print(p.device)
PY
}

# 0x6868: normal mode, 0x1915: DFU bootloader. The CLI path works where the raw frame of
# resource/tools/enter_dfu.py is unreliable on macOS.
if [ -z "$(find_port 1915)" ]; then
  APP_PORT="$(find_port 6868)"
  [ -n "$APP_PORT" ] || { echo "no Chameleon found"; exit 1; }
  "$S/clivenv/bin/python" - "$ROOT/software/script" "$APP_PORT" <<'PY' || true
import sys, time
sys.path.insert(0, sys.argv[1])
import chameleon_com, chameleon_cmd
com = chameleon_com.ChameleonCom()
com.open(sys.argv[2])
chameleon_cmd.ChameleonCMD(com).enter_bootloader()
time.sleep(4)
PY
fi
PORT=""
for i in $(seq 1 20); do
  PORT="$(find_port 1915)"
  [ -n "$PORT" ] && break
  sleep 1
done
[ -n "$PORT" ] || { echo "device did not enter DFU"; exit 1; }
"$S/nrfvenv/bin/nrfutil" dfu usb-serial -pkg ultra-dfu-app.zip -p "${PORT/cu./tty.}" -cd 6 -t 60 2>&1 | tail -2
sleep 6
