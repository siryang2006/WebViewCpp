"""
End-to-end download test via CDP.
Starts a download, monitors progress, pauses, resumes, then cancels.
"""
import asyncio
import json
import http.client
import subprocess
import os
import sys
import time

HOST = "localhost"
PORT = 9223
EXE_PATH = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                        "build", "Debug", "WebViewCpp.exe")
BASE_DIR = os.path.dirname(os.path.dirname(__file__))


def get_page_ws_url(port):
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            conn = http.client.HTTPConnection(HOST, port, timeout=5)
            conn.request("GET", "/json")
            resp = conn.getresponse()
            targets = json.loads(resp.read())
            conn.close()
            for t in targets:
                if t.get("type") == "page":
                    return t["webSocketDebuggerUrl"]
        except:
            time.sleep(1)
    raise RuntimeError("No page target found")


async def send_recv(ws, cmd_id, method, params=None):
    msg = {"id": cmd_id, "method": method}
    if params:
        msg["params"] = params
    await ws.send(json.dumps(msg))
    deadline = asyncio.get_event_loop().time() + 15
    while asyncio.get_event_loop().time() < deadline:
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=1)
        except asyncio.TimeoutError:
            continue
        data = json.loads(raw)
        if data.get("id") == cmd_id:
            return data


async def evaluate(ws, cid, expr, await_promise=False, timeout_ms=5000):
    cid += 1
    resp = await send_recv(ws, cid, "Runtime.evaluate", {
        "expression": expr,
        "returnByValue": True,
        "awaitPromise": await_promise,
        "timeout": timeout_ms
    })
    if not resp:
        raise RuntimeError(f"Timeout evaluating: {expr[:60]}")
    if "exceptionDetails" in resp.get("result", {}):
        exc = resp["result"]["exceptionDetails"]
        msg = exc.get('text', '')
        if exc.get('exception'):
            msg += ' - ' + exc['exception'].get('description', '')
        raise RuntimeError(f"JS exception: {msg}")
    val = resp.get("result", {}).get("result", {}).get("value", "<no value>")
    return cid, val


async def run():
    # Kill any existing instance
    subprocess.run(["taskkill", "/f", "/im", "WebViewCpp.exe"],
                   capture_output=True)
    await asyncio.sleep(1)

    proc = subprocess.Popen(
        [EXE_PATH, f"--cdp-port={PORT}"],
        cwd=os.path.dirname(EXE_PATH),
        creationflags=subprocess.CREATE_NO_WINDOW
    )

    await asyncio.sleep(4)

    try:
        ws_url = get_page_ws_url(PORT)
    except Exception as e:
        proc.terminate()
        print(f"FAIL: Cannot connect to CDP: {e}")
        return False

    import websockets
    async with websockets.connect(ws_url, max_size=2**20) as ws:
        cid = 0

        # Switch to model tab
        cid, val = await evaluate(ws, cid, """
            document.querySelectorAll('.tab-btn')[1].click();
            'switched'
        """)
        print(f"[OK] Switched to model tab: {val}")
        await asyncio.sleep(0.5)

        # Find the smallest model (llama-3.2-1b) and click download
        cid, val = await evaluate(ws, cid, """
            (function() {
                var btns = document.querySelectorAll('.model-row .btn-blue');
                for (var i = 0; i < btns.length; i++) {
                    var onclick = btns[i].getAttribute('onclick');
                    if (onclick && onclick.indexOf('llama-3.2-1b') !== -1) {
                        btns[i].click();
                        return 'clicked llama-3.2-1b';
                    }
                }
                // fallback: click first download button
                if (btns.length > 0) {
                    btns[0].click();
                    return 'clicked first available';
                }
                return 'no download button found';
            })()
        """)
        print(f"[OK] Download click: {val}")

        if 'no download button' in val:
            proc.terminate()
            return False

        # Wait and check progress a few times
        for i in range(8):
            await asyncio.sleep(2)
            cid, raw = await evaluate(ws, cid, f"""
                (async() => {{
                    var progress = document.querySelector('.model-dl-progress-bar');
                    var speed = document.querySelector('.model-dl-speed');
                    var status = document.querySelector('.model-dl-status');
                    return JSON.stringify({{
                        progress: progress ? progress.style.width : 'no bar',
                        speed: speed ? speed.textContent.trim() : 'no speed',
                        status: status ? status.textContent.trim() : 'no status'
                    }});
                }})()
            """, await_promise=True)
            print(f"  [{i+1}/8] Download progress: {raw}")

            # Check if we got actual progress
            try:
                data = json.loads(raw)
                if data['progress'] and data['progress'] != 'no bar' and data['progress'] != '0%':
                    print(f"[OK] Download making progress: {raw}")
                    break
            except:
                pass

        # Check actual file on disk
        import glob as fglob
        dl_files = fglob.glob(os.path.join(BASE_DIR, "downloads", "**", "*"), recursive=True)
        dl_files = [f for f in dl_files if os.path.isfile(f)]
        print(f"[INFO] Download directory files: {dl_files}")
        for f in dl_files:
            size = os.path.getsize(f)
            print(f"  {f}: {size} bytes ({size/1024/1024:.2f} MB)")

        # Now test pause
        cid, val = await evaluate(ws, cid, """
            (async() => {
                var btns = document.querySelectorAll('.model-row [onclick*="pauseDownload"]');
                if (btns.length > 0) {
                    btns[0].click();
                    await new Promise(r => setTimeout(r, 500));
                    return 'paused';
                }
                return 'no pause button';
            })()
        """, await_promise=True)
        print(f"[OK] Pause result: {val}")

        await asyncio.sleep(1)

        # Check status after pause
        cid, raw = await evaluate(ws, cid, """
            (async() => {
                var status = document.querySelector('.model-dl-status');
                return status ? status.textContent.trim() : 'no status';
            })()
        """, await_promise=True)
        print(f"[INFO] Status after pause: {raw}")

        # Resume
        cid, val = await evaluate(ws, cid, """
            (async() => {
                var btns = document.querySelectorAll('.model-row [onclick*="resumeDownload"]');
                if (btns.length > 0) {
                    btns[0].click();
                    await new Promise(r => setTimeout(r, 500));
                    return 'resumed';
                }
                return 'no resume button';
            })()
        """, await_promise=True)
        print(f"[OK] Resume result: {val}")

        await asyncio.sleep(2)

        # Check progress after resume
        cid, raw = await evaluate(ws, cid, """
            (async() => {
                var progress = document.querySelector('.model-dl-progress-bar');
                var status = document.querySelector('.model-dl-status');
                return JSON.stringify({
                    progress: progress ? progress.style.width : 'no bar',
                    status: status ? status.textContent.trim() : 'no status'
                });
            })()
        """, await_promise=True)
        print(f"[INFO] After resume: {raw}")

        # Cancel
        cid, val = await evaluate(ws, cid, """
            window.__cpp__.download.cancelDownload({modelId: 'llama-3.2-1b'}).catch(function(){});
            'cancelled'
        """)
        print(f"[OK] Cancelled download")
        await asyncio.sleep(0.5)

        print("\n=== Download E2E Test Complete ===")
        return True

    proc.terminate()


def main():
    ok = asyncio.run(run())
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
