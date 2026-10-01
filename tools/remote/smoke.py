#!/usr/bin/env python3
"""Smoke test for the remote against a live set (docs/REMOTE.md).

    python3 tools/remote/smoke.py http://<set-address>/

Checks: GET / (gzip page), GET /channels.json (parses, has current + channels with panels),
WebSocket slots (p 0, p 1, p -1 + full), ch <n> on connect, t + tunes and is broadcast.
Messages may arrive in any order, so each check collects for a moment and looks for what
it needs instead of asserting a fixed sequence.
"""
import asyncio, gzip, json, sys, urllib.request
import websockets

if len(sys.argv) < 2: sys.exit("usage: smoke.py http://<set-address>/")
BASE = sys.argv[1].rstrip("/")
WS = BASE.replace("http://", "ws://") + "/ws"

def get(path, gz=False):
    req = urllib.request.Request(BASE + path, headers={"Accept-Encoding": "gzip"} if gz else {})
    with urllib.request.urlopen(req, timeout=5) as r:
        body = r.read()
        return r.status, r.headers.get("Content-Encoding"), body

async def collect(ws, secs=1.0):
    out = []
    try:
        while True:
            out.append(await asyncio.wait_for(ws.recv(), secs))
    except (asyncio.TimeoutError, websockets.ConnectionClosed):
        pass
    return out

def need(msgs, want, tag):
    ok = all(any(m == w or m.startswith(w) for m in msgs) for w in want)
    print(f"  {tag}: {msgs} -> {'ok' if ok else 'MISSING ' + str(want)}")
    if not ok: raise SystemExit(f"smoke: FAIL at {tag}")

async def main():
    st, enc, body = get("/", gz=True)
    html = gzip.decompress(body) if enc == "gzip" else body
    assert st == 200 and b"<" in html[:200], "GET / failed"
    print(f"GET / : {st}, {enc}, {len(body)} B -> {len(html)} B")
    st, _, body = get("/channels.json")
    ch = json.loads(body)
    assert "current" in ch and ch["channels"] and all("panel" in c for c in ch["channels"]), "channels.json shape"
    print(f"GET /channels.json : {len(ch['channels'])} channels, current {ch['current']}")
    a = await websockets.connect(WS); need(await collect(a), ["p 0", "ch ", "s "], "ws1 connect")
    b = await websockets.connect(WS); need(await collect(b), ["p 1", "ch "], "ws2 connect")
    c = await websockets.connect(WS); need(await collect(c), ["p -1", "full"], "ws3 refused")
    await a.send("t +"); need(await collect(b, 1.5), ["ch "], "ws2 sees t + broadcast")
    await a.send(f"t {ch['current']}"); await collect(a, 1.0)       # leave the set where we found it
    await a.send("h"); await b.close(); await asyncio.sleep(0.3)
    need(await collect(a), ["s 1"], "ws1 sees ws2 leave")
    await a.close()
    print("smoke: OK")

asyncio.run(main())
