"""
CDP Integration Tests for WebViewCpp.

Comprehensive tests covering page structure, button existence,
__cpp__ object structure, sync method calls, properties, Worker
lifecycle, and button click event handling via CDP.

Usage:
  python tests/test_cdp.py
"""

import asyncio
import json
import http.client
import subprocess
import os
import sys
import time

HOST = "localhost"
PORT = 9222
EXE_PATH = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                        "build", "Debug", "WebViewCpp.exe")
TIMEOUT_SEC = 30


class TestFailed(Exception):
    pass


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


async def send_recv(ws, cmd_id, method, params=None):
    msg = {"id": cmd_id, "method": method}
    if params:
        msg["params"] = params
    await ws.send(json.dumps(msg))
    deadline = asyncio.get_event_loop().time() + 10
    while asyncio.get_event_loop().time() < deadline:
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=1)
        except asyncio.TimeoutError:
            continue
        data = json.loads(raw)
        if data.get("id") == cmd_id:
            return data


async def evaluate(ws, cid, expr, await_promise=False, timeout_ms=3000):
    cid += 1
    resp = await send_recv(ws, cid, "Runtime.evaluate", {
        "expression": expr,
        "returnByValue": True,
        "awaitPromise": await_promise,
        "timeout": timeout_ms
    })
    if not resp:
        raise TestFailed(f"Timeout evaluating: {expr[:60]}")
    if "exceptionDetails" in resp.get("result", {}):
        exc = resp["result"]["exceptionDetails"]
        msg = exc.get('text', '')
        if exc.get('exception'):
            msg += ' - ' + exc['exception'].get('description', '')
        raise TestFailed(f"JS exception: {msg}")
    val = resp.get("result", {}).get("result", {}).get("value", "<no value>")
    return cid, val


async def run_cdp_tests():
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
        raise TestFailed(f"Cannot connect to CDP: {e}")

    print(f"CDP connected: {ws_url}")

    import websockets
    async with websockets.connect(ws_url, max_size=2**20) as ws:
        cid = 0
        passed = 0
        failed = 0

        def check(name, ok, detail=""):
            nonlocal passed, failed
            if ok:
                passed += 1
                print(f"  [PASS] {name}")
            else:
                failed += 1
                print(f"  [FAIL] {name} - {detail}")

        # ========== Page Structure ==========

        # Page title
        cid, val = await evaluate(ws, cid, "document.title")
        check("Page title", val == "C++ / JS Binding Demo",
              f"got '{val}'")

        # Sections
        for s in ["全局单例", "JS new 创建 C++ 实例", "异步方法", "C++ calls JS"]:
            cid, val = await evaluate(ws, cid,
                f"document.body.innerHTML.includes('{s}')")
            check(f"Section: {s}", val is True)

        # Buttons
        for expected_text in [
            "math.add(10, 20)",
            "math.version & math.pi",
            'new Worker("Alice", 5)',
            'new Worker("Bob", 10)',
            "destroy worker1",
            'file.read("config.json")',
            "Register JS callback",
        ]:
            cid, val = await evaluate(ws, cid,
                f"Array.from(document.querySelectorAll('button')).some(b => b.textContent.trim() === '{expected_text}')")
            check(f"Button: {expected_text}", val is True)

        # Result divs
        for d in ["sync-result", "worker-result", "async-result", "cpp2js-result"]:
            cid, val = await evaluate(ws, cid,
                f"document.getElementById('{d}') !== null")
            check(f"Result div: #{d}", val is True)

        # ========== __cpp__ Object Structure ==========

        cid, val = await evaluate(ws, cid,
            "JSON.stringify(Object.keys(window.__cpp__ || {}))")
        cpp_keys = json.loads(val) if val else []
        check("__cpp__ keys",
              "math" in cpp_keys and "file" in cpp_keys and "Worker" in cpp_keys,
              f"got {cpp_keys}")

        # ========== Math Service Methods & Properties ==========

        for method in ["add", "multiply", "slow_add", "fetch_data"]:
            cid, val = await evaluate(ws, cid,
                f"typeof window.__cpp__.math.{method}")
            check(f"math.{method} is function", val == "function")

        # Math properties
        cid, val = await evaluate(ws, cid,
            "typeof Object.getOwnPropertyDescriptor(window.__cpp__.math, 'version')?.get")
        check("math.version has getter", val == "function")

        cid, val = await evaluate(ws, cid,
            "typeof Object.getOwnPropertyDescriptor(window.__cpp__.math, 'pi')?.get")
        check("math.pi has getter", val == "function")

        # ========== File Service Methods ==========

        for method in ["read", "write"]:
            cid, val = await evaluate(ws, cid,
                f"typeof window.__cpp__.file.{method}")
            check(f"file.{method} is function", val == "function")

        # ========== Worker Factory ==========

        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.Worker")
        check("Worker constructor is function", val == "function")

        # ========== JS Runtime Bindings ==========

        for bind_name in [
            "__webview_sync_call__",
            "__webview_async_call__",
            "__webview_get_property__",
            "__webview_set_property__",
            "__webview_create__",
            "__webview_destroy__",
            "__webview_list_types__",
            "__register_cb__",
            "__cpp_result__",
        ]:
            cid, val = await evaluate(ws, cid,
                f"typeof window['{bind_name}']")
            check(f"JS binding: {bind_name}", val == "function",
                  f"got typeof={val}")

        # __webview_list_types__
        cid, val = await evaluate(ws, cid,
            "JSON.stringify({t: typeof window.__webview_list_types__, v: String(window.__webview_list_types__)})")
        print(f"  DIAG: list_types type+value = {val}")
        cid, val = await evaluate(ws, cid,
            "(async() => JSON.stringify(await window.__webview_list_types__()))()",
            await_promise=True)
        check("__webview_list_types__ returns ['Worker']",
              val == '["Worker"]', f"got {val}")

        # __registered_cbs__ initial state
        cid, val = await evaluate(ws, cid,
            "JSON.stringify(Object.keys(window.__registered_cbs__ || {}))")
        check("__registered_cbs__ is empty initially",
              val == "[]", f"got {val}")

        # __pending_callbacks__ initial state
        cid, val = await evaluate(ws, cid,
            "JSON.stringify(Object.keys(window.__pending_callbacks__ || {}))")
        check("__pending_callbacks__ is empty initially",
              val == "[]", f"got {val}")

        # ========== Sync Method Call Results (await) ==========

        cid, val = await evaluate(ws, cid,
            "(async() => await window.__cpp__.math.add(10, 20))()",
            await_promise=True)
        check("math.add(10, 20) = 30",
              val == 30, f"got {val}")

        cid, val = await evaluate(ws, cid,
            "(async() => await window.__cpp__.math.multiply(6, 7))()",
            await_promise=True)
        check("math.multiply(6, 7) = 42",
              val == 42, f"got {val}")

        cid, val = await evaluate(ws, cid,
            "(async() => await window.__cpp__.math.version)()",
            await_promise=True)
        check("math.version = '1.0.0'",
              val == "1.0.0", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "(async() => { var p = await window.__cpp__.math.pi; return Math.abs(p - 3.14159) < 0.001; })()",
            await_promise=True)
        check("math.pi ~ 3.14159", val is True, f"got {val}")

        # ========== Worker Instance Lifecycle ==========

        # Diagnostic: inspect __webview__ bindings and try create
        cid, val = await evaluate(ws, cid,
            "typeof window.__webview_create__")
        check("__webview_create__ exists", val == "function", f"got {val}")
        cid, val = await evaluate(ws, cid,
            "typeof window.__webview__.call")
        check("__webview__.call exists", val == "function", f"got {val}")
        cid, val = await evaluate(ws, cid,
            "typeof window.__webview__.onReply")
        check("__webview__.onReply exists", val == "function", f"got {val}")

        # Diagnostic: is it the await or the call that hangs?
        cid, val = await evaluate(ws, cid, """
            var p = window.__webview_create__("Worker", JSON.stringify(["Alice",5]));
            typeof p
        """)
        print(f"\n  DIAG: __webview_create__() returned: typeof={val}")
        cid, val = await evaluate(ws, cid, """
            (async () => {
                var d = {};
                try {
                    var info = await window.__webview_create__("Worker", JSON.stringify(["Alice",5]));
                    d.ok = true;
                    d.info = JSON.stringify(info);
                } catch(e) {
                    d.ok = false;
                    d.err = e.message;
                }
                return JSON.stringify(d);
            })()
        """, await_promise=True, timeout_ms=5000)
        print(f"  DIAG: await __webview_create__ result = {val}")

        # Test 1: Create using __webview_create__ + makeProxy directly (simulate factory)
        cid, val = await evaluate(ws, cid, """
            (async () => {
                var info = await window.__webview_create__("Worker", JSON.stringify(["Alice",5]));
                return { infoOk: !!info.id, id: info.id };
            })()
        """, await_promise=True, timeout_ms=5000)
        print(f"  DIAG: direct create = {val}")
        check("Direct create works", val.get("infoOk") is True, f"got {val}")

        # Bypass factory entirely: test direct __webview_create__ + __webview_sync_call__
        cid, val = await evaluate(ws, cid, """
            (async () => {
                var info = await window.__webview_create__("Worker", JSON.stringify(["Alice",5]));
                var iid = info.id;
                // Call getName via __webview_sync_call__ directly
                var name = await window.__webview_sync_call__(iid, "getName", "[]");
                var pri  = await window.__webview_sync_call__(iid, "getPriority", "[]");
                var sp   = await window.__webview_sync_call__(iid, "setPriority", "[8]");
                var pri2 = await window.__webview_sync_call__(iid, "getPriority", "[]");
                return { name: name, pri: pri, sp: sp, pri2: pri2, iid: iid };
            })()
        """, await_promise=True, timeout_ms=5000)
        print(f"  DIAG: direct sync_call = {val}")
        check("Direct __webview_sync_call__ on instance works",
              val.get("name") == "Alice" and val.get("pri2") == 8,
              f"got {val}")
        check("Worker('Alice',5).getName() = 'Alice'",
              val.get("name") == "Alice", f"got {val}")
        check("Worker('Alice',5).getPriority() = 5",
              val.get("pri") == 5, f"got {val}")
        check("Worker('Alice',5).setPriority(8) = 8",
              val.get("sp") == 8, f"got {val}")
        check("Worker('Alice',5).getPriority after set = 8",
              val.get("pri2") == 8, f"got {val}")

        # Exercise the factory + Proxy path end-to-end (regression: async
        # factory returning a Proxy must not be treated as a thenable).
        cid, val = await evaluate(ws, cid, """
            (async () => {
                var r = {};
                try {
                    var w = await window.__cpp__.Worker("Alice", 5);
                    r.step = 'created';
                    r.name = await w.getName();          r.step = 'getName';
                    r.sp = await w.setPriority(8);        r.step = 'setPriority';
                    r.pri2 = await w.getPriority();       r.step = 'getPriority';
                    r.hasDestroy = typeof w.__destroy__;
                    var dw = await w.doWork("build");     r.step = 'doWork';
                    r.doWorkStatus = dw.status;
                } catch(e) { r.err = String(e); }
                return r;
            })()
        """, await_promise=True, timeout_ms=5000)
        print(f"  DIAG: factory proxy = {val}")
        check("Factory proxy chain has no error",
              val.get("err") is None, f"err at step '{val.get('step')}': {val.get('err')}")
        check("Factory Worker.getName() = 'Alice'",
              val.get("name") == "Alice", f"got {val}")
        check("Factory Worker.setPriority(8) = 8",
              val.get("sp") == 8, f"got {val}")
        check("Factory Worker.getPriority after set = 8",
              val.get("pri2") == 8, f"got {val}")
        check("Factory Worker.doWork() resolves",
              val.get("doWorkStatus") == "done", f"got {val}")
        check("Factory Worker.__destroy__ is function",
              val.get("hasDestroy") == "function", f"got {val}")

        # Destroy worker and verify cleanup
        cid, val = await evaluate(ws, cid, """
            (async () => {
                var w = await window.__cpp__.Worker("DestroyTest", 1);
                w.__destroy__();
                return {destroyed: w.__destroyed__, idType: typeof w.__id__};
            })()
        """, await_promise=True)
        check("Worker.__destroy__ marks destroyed",
              val.get("destroyed") is True, f"got {val}")
        check("Worker.__destroy__ keeps __id__",
              val.get("idType") == "string", f"got {val}")

        # Default args
        cid, val = await evaluate(ws, cid, """
            (async () => {
                var w = await window.__cpp__.Worker();
                var name = await w.getName();
                var pri = await w.getPriority();
                return {name: name, pri: pri};
            })()
        """, await_promise=True)
        check("Worker() default name = 'default'",
              val.get("name") == "default", f"got {val}")
        check("Worker() default priority = 0",
              val.get("pri") == 0, f"got {val}")

        # ========== Button Click DOM Updates ==========

        # math.add button
        cid, _ = await evaluate(ws, cid,
            "document.querySelector('button').click()")
        await asyncio.sleep(0.5)
        cid, val = await evaluate(ws, cid,
            "var el = document.getElementById('sync-result'); el ? el.textContent : ''")
        check("math.add click updates #sync-result",
              len(val) > 0, f"got: {repr(val[:80])}")

        # Register callback button
        cid, _ = await evaluate(ws, cid,
            "document.getElementById('btn-callback') ? document.getElementById('btn-callback').click() : document.querySelectorAll('button')[7].click()")
        await asyncio.sleep(0.3)
        cid, val = await evaluate(ws, cid,
            "var el = document.getElementById('cpp2js-result'); el ? el.textContent : ''")
        check("Register callback click updates #cpp2js-result",
              len(val) > 0, f"got: {repr(val[:80])}")

        # ========== Async Method Existence ==========

        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.math.slow_add")
        check("math.slow_add is async function", val == "function")

        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.math.fetch_data")
        check("math.fetch_data is async function", val == "function")

        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.file.write")
        check("file.write is async function", val == "function")

        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.file.read")
        check("file.read is async function", val == "function")

        # ========== Callback-style async: fn(a, b, function(err, result){}) ==========
        # 末尾传 JS 函数，C++ 完成后以 Node 约定 cb(err, result) 回调它。
        cid, val = await evaluate(ws, cid, """
            new Promise(function(resolve) {
                window.__cpp__.math.slow_add(1, 2, function(err, result) {
                    resolve(JSON.stringify({err: err, result: result}));
                });
            })
        """, await_promise=True, timeout_ms=5000)
        check("slow_add(1,2,cb) callback receives result",
              val == '{"err":null,"result":3}', f"got {val}")

        # 回调风格返回值是 Promise（兼容 await）；这里同时验证 await 链路一致。
        cid, val = await evaluate(ws, cid,
            "(async()=>{ return await window.__cpp__.math.slow_add(5, 7); })()",
            await_promise=True, timeout_ms=5000)
        check("slow_add(5,7) await (no cb) = 12", val == 12, f"got {val}")

        # ========== Reverse call: JS → C++.fire_event → C++ → registered JS cb ==========
        # 注册回调 → JS 调 C++.fire_event → C++ 反向调用 onCppEvent，验证整条闭环。
        cid, val = await evaluate(ws, cid, """
            new Promise(function(resolve) {
                window.__register_cb__("onCppEvent", function(args) {
                    resolve(JSON.stringify(args));
                });
                window.__cpp__.math.fire_event("test-evt");
            })
        """, await_promise=True, timeout_ms=5000)
        check("fire_event triggers registered onCppEvent",
              val == '{"event":"test-evt","source":"fire_event"}', f"got {val}")

        cid, val = await evaluate(ws, cid,
            '(async()=>{ var r = await window.__cpp__.math.fire_event("x"); return JSON.stringify(r); })()',
            await_promise=True, timeout_ms=5000)
        check("fire_event returns ack to caller",
              val == '{"event":"x","fired":true}', f"got {val}")

        # ========== Section Summary ==========
        print(f"\nCDP Tests: {passed} passed, {failed} failed, "
              f"{passed + failed} total")

    proc.terminate()
    return failed == 0


def main():
    if not os.path.exists(EXE_PATH):
        print(f"ERROR: {EXE_PATH} not found. Build first.")
        sys.exit(1)

    subprocess.run(["taskkill", "/f", "/im", "WebViewCpp.exe"],
                   capture_output=True)
    time.sleep(1)

    ok = asyncio.run(run_cdp_tests())
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
