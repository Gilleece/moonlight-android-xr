#!/usr/bin/env python3
"""Evaluate JavaScript in Steam's SharedJSContext over the CEF remote debugging port.
Usage: steam-cef.py 'JS expression'   (awaits promises; prints JSON result)"""
import json, os, socket, struct, sys, base64, urllib.request

def ws_connect(url):
    # ws://host:port/path
    rest = url[len('ws://'):]
    hostport, path = rest.split('/', 1)
    host, port = hostport.split(':')
    s = socket.create_connection((host, int(port)), timeout=30)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
    s.sendall(req.encode())
    buf = b''
    while b'\r\n\r\n' not in buf:
        chunk = s.recv(4096)
        if not chunk: raise RuntimeError('handshake failed')
        buf += chunk
    if b' 101 ' not in buf.split(b'\r\n')[0]:
        raise RuntimeError('handshake rejected: ' + buf.decode(errors='replace'))
    return s

def ws_send(s, text):
    data = text.encode()
    hdr = bytearray([0x81])
    n = len(data)
    if n < 126: hdr.append(0x80 | n)
    elif n < 65536: hdr.append(0x80 | 126); hdr += struct.pack('>H', n)
    else: hdr.append(0x80 | 127); hdr += struct.pack('>Q', n)
    mask = os.urandom(4)
    hdr += mask
    s.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

def recv_exact(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk: raise RuntimeError('connection closed')
        buf += chunk
    return buf

def ws_recv(s):
    while True:
        b1, b2 = recv_exact(s, 2)
        op = b1 & 0x0f
        n = b2 & 0x7f
        if n == 126: n = struct.unpack('>H', recv_exact(s, 2))[0]
        elif n == 127: n = struct.unpack('>Q', recv_exact(s, 8))[0]
        if b2 & 0x80: mask = recv_exact(s, 4)
        else: mask = None
        payload = recv_exact(s, n)
        if mask: payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        if op == 1: return payload.decode()
        if op == 8: raise RuntimeError('closed')
        # ignore ping/pong/binary

def evaluate(expr, title='SharedJSContext'):
    pages = json.load(urllib.request.urlopen('http://127.0.0.1:8080/json', timeout=5))
    page = next(p for p in pages if p.get('title') == title)
    s = ws_connect(page['webSocketDebuggerUrl'])
    ws_send(s, json.dumps({'id': 1, 'method': 'Runtime.evaluate',
                           'params': {'expression': expr, 'awaitPromise': True, 'returnByValue': True}}))
    while True:
        msg = json.loads(ws_recv(s))
        if msg.get('id') == 1:
            s.close()
            if 'error' in msg: raise RuntimeError(msg['error'])
            r = msg['result']
            if 'exceptionDetails' in r:
                raise RuntimeError(json.dumps(r['exceptionDetails'].get('exception', r['exceptionDetails']))[:2000])
            return r['result'].get('value')

if __name__ == '__main__':
    print(json.dumps(evaluate(sys.argv[1]), indent=1))
