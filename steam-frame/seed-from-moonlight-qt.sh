#!/bin/bash
# Optional: if Moonlight (the Qt desktop app, flatpak) on this machine is already
# paired with your PC, copy that pairing so Moonlight XR needs no PIN step.
# Writes ~/Games/MoonlightXR/seed/, which the launch wrapper applies on the first
# launch of a shortcut. Reads only your own Moonlight configuration.
set -e
CONF=$HOME/.var/app/com.moonlight_stream.Moonlight/config/Moonlight\ Game\ Streaming\ Project/Moonlight.conf
[ -f "$CONF" ] || CONF=$HOME/.config/Moonlight\ Game\ Streaming\ Project/Moonlight.conf
[ -f "$CONF" ] || { echo "no Moonlight Qt configuration found"; exit 1; }
OUT=$HOME/Games/MoonlightXR/seed
mkdir -p "$OUT" && chmod 700 "$OUT"
python3 - "$CONF" "$OUT" <<'PY'
import re, sys, os, json, sqlite3, subprocess
conf, out = sys.argv[1], sys.argv[2]
raw = open(conf, encoding='utf-8').read().replace('\\\n', '')
def val(key):
    m = re.search(r'^' + re.escape(key) + r'=(.*)$', raw, re.M)
    if not m: return None
    v = re.match(r'^"?@ByteArray\((.*)\)"?$', m.group(1), re.S).group(1)
    return v.replace('\\n', '\n').replace('\\\\', '\\')
def plain(key):
    m = re.search(r'^' + re.escape(key) + r'=(.*)$', raw, re.M)
    return m.group(1) if m else ''
cert, key = val('certificate'), val('key')
if not cert or not key: sys.exit('no client certificate in ' + conf)
open(os.path.join(out, 'client.crt'), 'w').write(cert.rstrip('\n') + '\n')
der = subprocess.run(['openssl', 'pkey', '-outform', 'DER'], input=key.encode(), capture_output=True, check=True).stdout
open(os.path.join(out, 'client.key'), 'wb').write(der)
open(os.path.join(out, 'uniqueid'), 'w').write('0123456789ABCDEF')
db = sqlite3.connect(os.path.join(out, 'computers4.db'))
db.execute("CREATE TABLE IF NOT EXISTS Computers(UUID TEXT PRIMARY KEY, ComputerName TEXT NOT NULL, Addresses TEXT NOT NULL, MacAddress TEXT, ServerCert TEXT)")
n = int(plain('hosts\\size') or 0)
for i in range(1, n + 1):
    p = 'hosts\\%d\\' % i
    uuid, name = plain(p + 'uuid'), plain(p + 'hostname')
    if not uuid: continue
    addr = {}
    for k, qt in (('local', 'localaddress'), ('remote', 'remoteaddress'), ('manual', 'manualaddress'), ('ipv6', 'ipv6address')):
        a = plain(p + qt); port = plain(p + qt.replace('address', 'port')) or '47989'
        if a: addr[k] = {'address': a, 'port': int(port)}
    srv = val(p + 'srvcert')
    srvder = subprocess.run(['openssl', 'x509', '-outform', 'DER'], input=srv.encode(), capture_output=True, check=True).stdout if srv else None
    db.execute("INSERT OR REPLACE INTO Computers VALUES (?,?,?,?,?)", (uuid, name, json.dumps(addr), None, sqlite3.Binary(srvder) if srvder else None))
    print('imported', name, uuid)
db.commit()
PY
chmod 600 "$OUT"/*
echo "pairing saved to $OUT; it is applied on the next first launch of a Moonlight XR shortcut"
