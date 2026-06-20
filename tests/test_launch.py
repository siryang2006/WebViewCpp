"""
Quick script: launch a model via CDP and report result.
Picks smallest working GGUF (dolphin-gemma2-2b or llama-3.2-1b).
"""
import asyncio, json, http.client, os, subprocess, sys, time

HOST, PORT = "localhost", 9222
EXE_PATH = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                        "build", "Debug", "WebViewCpp.exe")

def get_page_ws_url(port):
    conn = http.client.HTTPConnection(HOST, port, timeout=5)
    conn.request("GET", "/json")
    resp = conn.getresponse()
    targets = json.loads(resp.read())
    conn.close()
    for t in targets:
        if t.get("type") == "page":
            return t["webSocketDebuggerUrl"]
    raise RuntimeError("No page target found")

async def send_recv(ws, cmd_id, method, params=None, deadline_s=15):
    msg = {"id": cmd_id, "method": method}
    if params: msg["params"] = params
    await ws.send(json.dumps(msg))
    deadline = asyncio.get_event_loop().time() + deadline_s
    while asyncio.get_event_loop().time() < deadline:
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=1)
        except asyncio.TimeoutError:
            continue
        data = json.loads(raw)
        if data.get("id") == cmd_id:
            return data

async def evaluate(ws, cid, expr, await_promise=False, timeout_ms=60000):
    cid += 1
    resp = await send_recv(ws, cid, "Runtime.evaluate", {
        "expression": expr,
        "returnByValue": True,
        "awaitPromise": await_promise,
        "timeout": timeout_ms
    }, deadline_s=timeout_ms // 1000 + 15)
    if not resp:
        return cid, f"TIMEOUT"
    if "exceptionDetails" in resp.get("result", {}):
        exc = resp["result"]["exceptionDetails"]
        msg = exc.get('text', '')
        if exc.get('exception'):
            msg += ' - ' + exc['exception'].get('description', '')
        return cid, f"EXCEPTION: {msg}"
    val = resp.get("result", {}).get("result", {}).get("value", "<no value>")
    return cid, val

async def main():
    exe_dir = os.path.dirname(EXE_PATH)

    # Find smallest valid GGUF
    candidates = []
    downloads = os.path.join(exe_dir, "downloads")
    if os.path.isdir(downloads):
        for root, _dirs, files in os.walk(downloads):
            for fn in files:
                if fn.lower().endswith(".gguf"):
                    full = os.path.join(root, fn)
                    try:
                        size = os.path.getsize(full)
                    except OSError:
                        continue
                    if size < 1024 * 1024:
                        continue
                    rel = os.path.relpath(full, exe_dir)
                    model_id = os.path.basename(root)
                    candidates.append((size, model_id, rel))
    candidates.sort()
    if not candidates:
        print("No valid GGUF files found")
        return
    _size, model_id, gguf_rel = candidates[0]
    print(f"Using model: {model_id} ({_size} bytes)")
    print(f"  GGUF: {gguf_rel}")

    # Kill any stale webview/llama processes
    subprocess.run(f"taskkill /f /im WebViewCpp.exe 2>nul", shell=True, stderr=subprocess.DEVNULL)
    subprocess.run(f"taskkill /f /im llama-server.exe 2>nul", shell=True, stderr=subprocess.DEVNULL)
    await asyncio.sleep(2)

    # Start app with CDP
    proc = subprocess.Popen(
        [EXE_PATH, f"--cdp-port={PORT}"],
        cwd=exe_dir,
        creationflags=subprocess.CREATE_NO_WINDOW
    )
    await asyncio.sleep(4)

    try:
        ws_url = get_page_ws_url(PORT)
    except Exception as e:
        proc.terminate()
        print(f"Cannot connect to CDP: {e}")
        return

    import websockets
    async with websockets.connect(ws_url, max_size=2**20) as ws:
        cid = 0

        # Launch via chatService.startModel()
        print("\n=== Lauching model via chatService.startModel() ===")
        js = f"""
(async () => {{
    var r = await window.chatService.startModel({{
        modelId: '{model_id}',
        ggufPath: '{gguf_rel}',
        ctx: 4096,
        ngl: -1,
        threads: 4,
        flashAttn: true,
        thinking: false
    }});
    return JSON.stringify(r);
}})()
"""
        cid, val = await evaluate(ws, cid, js, await_promise=True, timeout_ms=120000)
        print(f"startModel result: {val}")

        try:
            r = json.loads(val) if isinstance(val, str) else {"raw": str(val)}
            if r.get("ok"):
                print(f"\n*** SUCCESS *** Model {model_id} started on port {r.get('data', {}).get('port', '?')}")
            else:
                print(f"\n*** FAILED *** {r.get('message', 'unknown error')}")
                print(f"Full: {json.dumps(r, ensure_ascii=False)}")
        except Exception as e:
            print(f"\n*** FAILED *** Could not parse result: {e}")

        # Get status
        cid, val = await evaluate(ws, cid,
            "window.__cpp__.chat.getStatus({}).then(function(r){return JSON.stringify(r);})",
            await_promise=True, timeout_ms=5000)
        print(f"getStatus: {val}")

        # Get metrics
        cid, val = await evaluate(ws, cid,
            "window.__cpp__.chat.getMetrics({}).then(function(r){return JSON.stringify(r);})",
            await_promise=True, timeout_ms=5000)
        print(f"getMetrics: {val}")

        # Now try UI-based launch via config modal for another model
        print("\n=== Trying UI-based launch for llama-3.2-1b via config modal ===")

        # Switch to model tab
        cid, val = await evaluate(ws, cid,
            "(function(){var btns=document.querySelectorAll('.tab-btn');btns[1].click();return 'switched';})()")
        print(f"Switch tab: {val}")
        await asyncio.sleep(0.5)

        # Find a model row with llama-3.2-1b and click detail button
        cid, val = await evaluate(ws, cid, """
(function(){
    var rows = document.querySelectorAll('.model-row');
    for (var i = 0; i < rows.length; i++) {
        var idEl = rows[i].querySelector('.model-id');
        if (idEl && idEl.textContent.trim() === 'llama-3.2-1b') {
            var btn = rows[i].querySelector('[onclick*=\"showDetail\"]');
            if (btn) { btn.click(); return 'detail opened'; }
        }
    }
    return 'model not found';
})()
""")
        print(f"Open detail: {val}")
        await asyncio.sleep(1)

        # Check detail page is open
        cid, val = await evaluate(ws, cid,
            "document.getElementById('detailPage').classList.contains('open')")
        print(f"Detail open: {val}")

        # Click launch button in detail page
        # The button says "▶ 立即启动" and calls showConfig(modelId)
        cid, val = await evaluate(ws, cid, """
(function(){
    var btns = document.querySelectorAll('.btn-green');
    for (var i = 0; i < btns.length; i++) {
        if (btns[i].textContent.indexOf('启动') >= 0) {
            btns[i].click();
            return 'launch clicked';
        }
    }
    return 'launch btn not found';
})()
""")
        print(f"Launch click: {val}")
        await asyncio.sleep(1)

        # Check config modal appeared
        cid, val = await evaluate(ws, cid,
            "document.getElementById('configOverlay').classList.contains('show')")
        print(f"Config modal: {val}")

        # Click the confirm button in config modal
        cid, val = await evaluate(ws, cid, """
(function(){
    var btn = document.getElementById('configConfirm');
    if (btn) { btn.click(); return 'confirm clicked'; }
    return 'confirm btn not found';
})()
""")
        print(f"Confirm click: {val}")

        # Wait and check result - poll for model status
        await asyncio.sleep(30)

        # Check status
        cid, val = await evaluate(ws, cid,
            "window.__cpp__.chat.getStatus({}).then(function(r){return JSON.stringify(r);})",
            await_promise=True, timeout_ms=10000)
        print(f"getStatus after UI launch: {val}")

        # Try chat with llama-3.2-1b
        print("\n=== Chat test with UI-launched model ===")
        cid, val = await evaluate(ws, cid, """
(async () => {
    var ib = document.getElementById('inputBox');
    var sb = document.getElementById('sendBtn');
    if (!ib || !sb) return JSON.stringify({error: 'input/send not found'});
    ib.value = 'Hello! Who are you?';
    ib.dispatchEvent(new Event('input', {bubbles: true}));
    sb.click();
    document.getElementById('chatArea').style.display = 'flex';
    var userEl = null;
    for (var i = 0; i < 20; i++) {
        var msgs = document.querySelectorAll('.msg.user .msg-bubble');
        if (msgs.length > 0) { userEl = msgs[msgs.length-1]; break; }
        await new Promise(function(r){ setTimeout(r, 200); });
    }
    var userText = userEl ? userEl.textContent : 'TIMEOUT_USER';
    var botEl = null;
    for (var i = 0; i < 100; i++) {
        var bots = document.querySelectorAll('.msg.bot .msg-bubble');
        if (bots.length > 0) {
            var bubble = bots[bots.length-1];
            var txt = bubble.textContent.trim();
            if (txt.length > 0 && txt.indexOf('\u258b') === -1) {
                botEl = bubble; break;
            }
        }
        await new Promise(function(r){ setTimeout(r, 300); });
    }
    var botText = botEl ? botEl.textContent : 'TIMEOUT_BOT';
    return JSON.stringify({ userText: userText, botText: botText });
})()
""", await_promise=True, timeout_ms=120000)
        print(f"Chat result: {str(val)[:300]}")

    proc.terminate()
    proc.wait()

if __name__ == '__main__':
    asyncio.run(main())
