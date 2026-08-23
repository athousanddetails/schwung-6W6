#!/usr/bin/env python3
"""Force a Schwung chain slot to reload its synth module.

WHY THIS EXISTS. Deploying a new dsp.so does not change what the device is
running. The chain host dlopen()s the plugin into the shim (inside
MoveOriginal); the atomic mv in deploy.sh swaps the directory entry while the
running process keeps the old inode mapped. `kill shadow_ui` does not help —
shadow_ui is a different process. Worse, an on-device loadtest dlopens the
file itself, so it passes against code nobody is hearing.

Re-writing the slot's module key makes the chain host unload the position and
dlopen a fresh instance: exactly what re-picking the synth in the UI does.

    ./scripts/reload_slot.py <host> [slot] [module-id]

Standard library only (the Mac has no toolchain and no venv here).
"""
import base64, json, os, socket, struct, sys, time

def send(sock, obj):
    payload = json.dumps(obj).encode()
    header = bytearray([0x81])
    n = len(payload)
    if n < 126:
        header.append(0x80 | n)
    elif n < 65536:
        header.append(0x80 | 126); header += struct.pack(">H", n)
    else:
        header.append(0x80 | 127); header += struct.pack(">Q", n)
    mask = os.urandom(4)
    header += mask
    sock.sendall(bytes(header) + bytes(c ^ mask[i % 4] for i, c in enumerate(payload)))

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "move.local"
    slot = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    module = sys.argv[3] if len(sys.argv) > 3 else "6w6"

    key = base64.b64encode(os.urandom(16)).decode()
    try:
        sock = socket.create_connection((host, 7700), timeout=6)
    except OSError as e:
        print("reload: cannot reach schwung-manager on %s:7700 (%s)" % (host, e))
        return 1
    sock.sendall((
        "GET /ws/remote-ui HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n" % (host, key)).encode())

    head = b""
    sock.settimeout(6)
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(1)
        if not chunk:
            print("reload: websocket handshake closed early")
            return 1
        head += chunk
    if b" 101" not in head.split(b"\r\n")[0]:
        print("reload: websocket upgrade refused:", head.split(b"\r\n")[0].decode(errors="replace"))
        return 1

    send(sock, {"type": "subscribe", "slot": slot})
    time.sleep(0.3)
    send(sock, {"type": "set_param", "slot": slot, "key": "synth:module", "value": module})
    time.sleep(1.5)
    sock.close()
    print("reload: asked slot %d to re-load '%s' — the new dsp.so is now the running one" % (slot, module))
    return 0

if __name__ == "__main__":
    sys.exit(main())
