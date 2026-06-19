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


def safe_print(s):
    """控制台可能是 GBK 编码，无法打印 emoji；统一转 ascii 安全输出。"""
    print(str(s).encode('ascii', 'replace').decode())


def pick_smallest_gguf():
    """在 build/Debug/downloads 下查找体积最小的有效 gguf 文件，
    返回 (modelId, 相对 exe 目录的路径)；找不到返回 None。
    用于真实启动模型测试——选最小的以缩短加载时间。
    gguf 文件大于 1MB 才算有效（过滤占位/损坏文件）。"""
    exe_dir = os.path.dirname(EXE_PATH)
    downloads = os.path.join(exe_dir, "downloads")
    if not os.path.isdir(downloads):
        return None
    candidates = []
    for root, _dirs, files in os.walk(downloads):
        for fn in files:
            if fn.lower().endswith(".gguf"):
                full = os.path.join(root, fn)
                try:
                    size = os.path.getsize(full)
                except OSError:
                    continue
                if size < 1024 * 1024:  # 过滤 < 1MB 的占位/损坏文件
                    continue
                rel = os.path.relpath(full, exe_dir)
                model_id = os.path.basename(root)
                candidates.append((size, model_id, rel))
    if not candidates:
        return None
    candidates.sort()
    _size, model_id, rel = candidates[0]
    return (model_id, rel)


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
                print(f"  [FAIL] {name} - {detail}".encode('ascii', 'replace').decode())

        # ========== Page Structure ==========

        # Page title
        cid, val = await evaluate(ws, cid, "document.title")
        check("Page title", val == "FlowyAIPC",
              f"got '{val}'")

        # FlowyAIPC structure: topbar tabs
        cid, val = await evaluate(ws, cid,
            "document.querySelector('.topbar-logo-text')?.textContent")
        check("topbar logo text", val == "FlowyAIPC", f"got '{val}'")

        cid, val = await evaluate(ws, cid,
            "document.querySelectorAll('.tab-btn').length")
        check("topbar tabs count", val == 2, f"got {val}")

        cid, val = await evaluate(ws, cid,
            "Array.from(document.querySelectorAll('.tab-btn')).map(b=>b.textContent.trim())")
        check("topbar tabs labels", val == ["应用", "模型"], f"got {val}")

        # Feature cards on app page
        cid, val = await evaluate(ws, cid,
            "document.querySelectorAll('.feature-card').length")
        check("feature cards rendered", val >= 12, f"got {val}")

        # New chat button (check by id, text may include emoji)
        cid, val = await evaluate(ws, cid,
            "document.getElementById('newChatBtn') !== null")
        check("new chat button exists", val is True)

        # Input box
        cid, val = await evaluate(ws, cid,
            "document.getElementById('inputBox') !== null")
        check("input box exists", val is True)

        # Model page (hidden initially)
        cid, val = await evaluate(ws, cid,
            "document.getElementById('modelPage') !== null")
        check("model page exists", val is True)
        cid, val = await evaluate(ws, cid,
            "!document.getElementById('modelPage').classList.contains('active')")
        check("model page hidden initially", val is True)

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
                // info 应该是 {id, type, sync, async, props}
                return { infoOk: !!(info && info.id), id: info ? info.id : null };
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

        # ========== UI Interaction Tests ==========

        # Feature card click - should add active class
        cid, val = await evaluate(ws, cid,
            "(function(){var c=document.querySelectorAll('.feature-card');c[1].click();return c[1].classList.contains('active');})()")
        check("feature card click adds active class", val)

        # Send message
        cid, val = await evaluate(ws, cid,
            "(function(){var i=document.getElementById('inputBox');i.value='hello';i.dispatchEvent(new Event('input'));document.getElementById('sendBtn').click();return 'sent';})()")
        check("send message click works", val == "sent")
        await asyncio.sleep(1.5)
        cid, val = await evaluate(ws, cid,
            "document.querySelectorAll('.msg.user').length")
        check("user message appears in chat", val >= 1)

        # Model tab - switch to model page
        cid, val = await evaluate(ws, cid,
            "(function(){document.querySelectorAll('.tab-btn')[1].click();return 'switched';})()")
        check("model tab click switches tab", val == "switched")
        await asyncio.sleep(0.5)
        cid, val = await evaluate(ws, cid,
            "document.getElementById('modelPage').classList.contains('active')")
        check("model page becomes active", val)
        cid, val = await evaluate(ws, cid,
            "document.querySelectorAll('.model-row').length")
        check("model rows rendered", val >= 1)

        # Model detail page - click ⋯ button
        cid, val = await evaluate(ws, cid,
            "(function(){document.querySelectorAll('[onclick*=showDetail]')[0].click();return 'detail opened';})()")
        check("detail page opens", val == "detail opened")
        await asyncio.sleep(0.3)
        cid, val = await evaluate(ws, cid,
            "document.getElementById('detailPage').classList.contains('open')")
        check("detail page has open class", val)

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

        # ========== DownloadService Tests ==========
        # 验证 C++ DownloadService 对象存在
        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.download")
        check("window.__cpp__.download exists",
              val == "object", f"got {val}")

        # 验证 DownloadService 方法存在
        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.download.startDownload")
        check("download.startDownload is function",
              val == "function", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.download.pauseDownload")
        check("download.pauseDownload is function",
              val == "function", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.download.resumeDownload")
        check("download.resumeDownload is function",
              val == "function", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.download.cancelDownload")
        check("download.cancelDownload is function",
              val == "function", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.download.getProgress")
        check("download.getProgress is function",
              val == "function", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.download.getSpeed")
        check("download.getSpeed is function",
              val == "function", f"got {val}")

        # 验证 JS DownloadService 对象存在
        cid, val = await evaluate(ws, cid,
            "typeof window.downloadService")
        check("window.downloadService exists",
              val == "object", f"got {val}")

        # 验证 JS DownloadService 方法存在
        cid, val = await evaluate(ws, cid,
            "typeof window.downloadService.startDownload")
        check("downloadService.startDownload is function",
              val == "function", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "typeof window.downloadService.pauseDownload")
        check("downloadService.pauseDownload is function",
              val == "function", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "typeof window.downloadService.resumeDownload")
        check("downloadService.resumeDownload is function",
              val == "function", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "typeof window.downloadService.cancelDownload")
        check("downloadService.cancelDownload is function",
              val == "function", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "typeof window.downloadService.getProgress")
        check("downloadService.getProgress is function",
              val == "function", f"got {val}")

        cid, val = await evaluate(ws, cid,
            "typeof window.downloadService.getSpeed")
        check("downloadService.getSpeed is function",
              val == "function", f"got {val}")

        # 验证 pauseDownload 无任务时返回错误
        cid, val = await evaluate(ws, cid,
            '(async()=>{ try { var r = await window.__cpp__.download.pauseDownload({modelId:"nonexistent"}); return JSON.stringify(r); } catch(e) { return JSON.stringify({ok:false, error:String(e)}); } })()',
            await_promise=True, timeout_ms=5000)
        check("pauseDownload(no task) returns error",
              '"ok":false' in val or '"ok": false' in val, f"got {val}")

        # 验证 resumeDownload 无任务时返回错误
        cid, val = await evaluate(ws, cid,
            '(async()=>{ try { var r = await window.__cpp__.download.resumeDownload({modelId:"nonexistent"}); return JSON.stringify(r); } catch(e) { return JSON.stringify({ok:false, error:String(e)}); } })()',
            await_promise=True, timeout_ms=5000)
        check("resumeDownload(no task) returns error",
              '"ok":false' in val or '"ok": false' in val, f"got {val}")

        # 验证 cancelDownload 无任务时返回错误
        cid, val = await evaluate(ws, cid,
            '(async()=>{ try { var r = await window.__cpp__.download.cancelDownload({modelId:"nonexistent"}); return JSON.stringify(r); } catch(e) { return JSON.stringify({ok:false, error:String(e)}); } })()',
            await_promise=True, timeout_ms=5000)
        check("cancelDownload(no task) returns error",
              '"ok":false' in val or '"ok": false' in val, f"got {val}")

        # 验证 getProgress 无任务时返回错误
        cid, val = await evaluate(ws, cid,
            '(async()=>{ try { var r = await window.__cpp__.download.getProgress({modelId:"nonexistent"}); return JSON.stringify(r); } catch(e) { return JSON.stringify({ok:false, error:String(e)}); } })()',
            await_promise=True, timeout_ms=5000)
        check("getProgress(no task) returns error",
              '"ok":false' in val or '"ok": false' in val, f"got {val}")

        # 验证 getSpeed 无任务时返回错误
        cid, val = await evaluate(ws, cid,
            '(async()=>{ try { var r = await window.__cpp__.download.getSpeed({modelId:"nonexistent"}); return JSON.stringify(r); } catch(e) { return JSON.stringify({ok:false, error:String(e)}); } })()',
            await_promise=True, timeout_ms=5000)
        check("getSpeed(no task) returns error",
              '"ok":false' in val or '"ok": false' in val, f"got {val}")

        # 验证模型页加载（检查是否有模型行渲染）
        cid, val = await evaluate(ws, cid, """
            document.querySelector('.tab-btn[data-tab="model"]').click();
            'clicked'
        """)
        check("click model tab", val == "clicked", f"got {val}")

        # 等待模型加载完成（config.read 可能需要一些时间）
        cid, val = await evaluate(ws, cid, """
            new Promise(function(resolve) {
                function check() {
                    var rows = document.querySelectorAll('.model-row');
                    if (rows.length > 0) {
                        resolve({rows: rows.length});
                    } else {
                        setTimeout(check, 200);
                    }
                }
                check();
            })
        """, await_promise=True, timeout_ms=8000)
        check("model rows rendered", val and val.get('rows', 0) > 0, f"got {val}")

        # 检查模型状态（可能是 downloaded 或 available）
        cid, val = await evaluate(ws, cid, """
            JSON.stringify(window.AppState.models.map(function(m) {
                return {id: m.id, status: m.status};
            }))
        """, await_promise=True, timeout_ms=3000)
        print(f"  DIAG: model statuses = {val}")
        check("models have status field", val and 'status' in val, f"got {val}")

        # Cancel any active download to clean up
        await evaluate(ws, cid, """
            window.__cpp__.download.cancelDownload({modelId: 'dolphin-gemma2-2b'}).catch(function(){});
        """, await_promise=False, timeout_ms=2000)

        # ========== ChatService Tests ==========
        # 验证 C++ ChatService 对象存在
        cid, val = await evaluate(ws, cid,
            "typeof window.__cpp__.chat")
        check("window.__cpp__.chat exists", val == "object", f"got {val}")

        # 验证 ChatService 方法存在
        for method in ["startModel", "stopModel", "chat", "getStatus", "getMetrics", "downloadServer"]:
            cid, val = await evaluate(ws, cid,
                f"typeof window.__cpp__.chat.{method}")
            check(f"chat.{method} is function", val == "function", f"got {val}")

        # 验证 JS ChatService 封装存在
        cid, val = await evaluate(ws, cid, "typeof window.chatService")
        check("window.chatService exists", val == "object", f"got {val}")

        for method in ["startModel", "stopModel", "chat", "getStatus", "getMetrics"]:
            cid, val = await evaluate(ws, cid,
                f"typeof window.chatService.{method}")
            check(f"chatService.{method} is function", val == "function", f"got {val}")

        # getStatus（无参）：未启动模型时返回 {status:'stopped', models:[]}
        cid, val = await evaluate(ws, cid,
            '(async()=>{ var r = await window.__cpp__.chat.getStatus(); return JSON.stringify(r); })()',
            await_promise=True, timeout_ms=5000)
        print(f"  DIAG: chat.getStatus = {val}")
        st = json.loads(val)
        check("chat.getStatus ok wrapper", st.get("ok") is True, f"got {val}")
        check("chat.getStatus idle => stopped with empty models",
              st["data"].get("status") == "stopped"
              and st["data"].get("models") == [], f"got {val}")

        # getMetrics（无参）：未启动时返回 {status:'stopped', models:[]}
        cid, val = await evaluate(ws, cid,
            '(async()=>{ var r = await window.__cpp__.chat.getMetrics(); return JSON.stringify(r); })()',
            await_promise=True, timeout_ms=5000)
        print(f"  DIAG: chat.getMetrics = {val}")
        mt = json.loads(val)
        check("chat.getMetrics ok wrapper", mt.get("ok") is True, f"got {val}")
        check("chat.getMetrics idle => empty models array",
              isinstance(mt["data"].get("models"), list) and mt["data"]["models"] == [],
              f"got {val}")

        # chatService.getAllMetrics() 封装：空闲时返回空数组
        cid, val = await evaluate(ws, cid,
            '(async()=>{ var arr = await window.chatService.getAllMetrics(); return JSON.stringify(arr); })()',
            await_promise=True, timeout_ms=5000)
        check("chatService.getAllMetrics returns array",
              isinstance(json.loads(val), list), f"got {val}")

        # startModel 缺少 modelId 时应返回 INVALID_ARGUMENTS（验证参数解包正确，
        # 不再抛 json type_error.306）
        cid, val = await evaluate(ws, cid,
            '(async()=>{ try { var r = await window.__cpp__.chat.startModel({}); return JSON.stringify(r); } catch(e) { return "EXC:" + String(e); } })()',
            await_promise=True, timeout_ms=5000)
        print(f"  DIAG: chat.startModel({{}}) = {val}")
        check("chat.startModel({}) rejects missing modelId without json exception",
              not val.startswith("EXC:") and json.loads(val).get("ok") is False
              and json.loads(val).get("code") == -4, f"got {val}")

        # startModel 带不存在的 gguf：参数被正确解包（不抛异常），返回结构化结果。
        cid, val = await evaluate(ws, cid,
            '(async()=>{ try { var r = await window.__cpp__.chat.startModel({modelId:"test-model", ggufPath:"nonexistent.gguf", ctx:2048, ngl:0, threads:2}); return JSON.stringify(r); } catch(e) { return "EXC:" + String(e); } })()',
            await_promise=True, timeout_ms=40000)
        print(f"  DIAG: chat.startModel(args) = {val}")
        check("chat.startModel(args) parses object args (no json exception)",
              not val.startswith("EXC:") and "ok" in json.loads(val), f"got {val}")

        # 通过 JS 封装层 chatService.startModel 验证同样的解包路径
        cid, val = await evaluate(ws, cid,
            '(async()=>{ try { var r = await window.chatService.startModel({modelId:"test-model2", ggufPath:"nonexistent.gguf"}); return JSON.stringify(r); } catch(e) { return "EXC:" + String(e); } })()',
            await_promise=True, timeout_ms=40000)
        print(f"  DIAG: chatService.startModel = {val}")
        check("chatService.startModel wrapper works (no json exception)",
              not val.startswith("EXC:") and "ok" in json.loads(val), f"got {val}")

        # ---- 真实启动模型（仅当存在小体积 gguf 文件时执行）----
        # 选一个最小的已下载模型做真实启动，验证：子进程拉起、getStatus 报告 running、
        # 单模型 getMetrics(modelId) 返回真实内存占用、无参 getMetrics 含该模型，
        # 最后能正常停止并清空指标。
        smallest = pick_smallest_gguf()
        if smallest:
            model_id, gguf_rel = smallest
            print(f"  DIAG: real launch model={model_id} gguf={gguf_rel}")
            launch_js = (
                '(async()=>{ try {'
                ' var r = await window.__cpp__.chat.startModel({modelId:"' + model_id + '",'
                ' ggufPath:"' + gguf_rel.replace('\\', '/') + '", ctx:512, ngl:0, threads:2, flashAttn:false});'
                ' return JSON.stringify(r); } catch(e) { return "EXC:" + String(e); } })()'
            )
            cid, val = await evaluate(ws, cid, launch_js,
                await_promise=True, timeout_ms=60000)
            print(f"  DIAG: real startModel = {val}")
            launch = json.loads(val) if not val.startswith("EXC:") else {}
            running = launch.get("ok") and launch.get("data", {}).get("status") == "running"
            check("real startModel launches llama-server (status running)",
                  running is True, f"got {val}")

            if running:
                launch_port = launch["data"].get("port")
                check("real startModel returns a port", isinstance(launch_port, int) and launch_port > 0,
                      f"got {val}")

                # getStatus(modelId) 应报告 running
                cid, val = await evaluate(ws, cid,
                    '(async()=>{ var r = await window.__cpp__.chat.getStatus({modelId:"' + model_id + '"}); return JSON.stringify(r); })()',
                    await_promise=True, timeout_ms=5000)
                print(f"  DIAG: getStatus(modelId) = {val}")
                d = json.loads(val)["data"]
                check("getStatus(modelId) reports running",
                      d.get("status") == "running" and d.get("modelId") == model_id, f"got {val}")
                check("getStatus(modelId) includes llama-server PID",
                      isinstance(d.get("pid"), int) and d.get("pid") > 0, f"got {val}")

                # getStatus（无参）应在 models[] 中包含该模型
                cid, val = await evaluate(ws, cid,
                    '(async()=>{ var r = await window.__cpp__.chat.getStatus(); return JSON.stringify(r); })()',
                    await_promise=True, timeout_ms=5000)
                models = json.loads(val)["data"].get("models", [])
                check("getStatus() lists running model in models[]",
                      any(m.get("modelId") == model_id for m in models), f"got {val}")

                # getMetrics(modelId)：真实内存占用 > 0
                cid, val = await evaluate(ws, cid,
                    '(async()=>{ var r = await window.__cpp__.chat.getMetrics({modelId:"' + model_id + '"}); return JSON.stringify(r); })()',
                    await_promise=True, timeout_ms=5000)
                print(f"  DIAG: getMetrics(modelId) = {val}")
                gd = json.loads(val)["data"]
                check("getMetrics(modelId) status ok", gd.get("status") == "ok", f"got {val}")
                check("getMetrics(modelId) reports real memory (>0 MB)",
                      (gd.get("memoryMB") or 0) > 0, f"got {val}")
                check("getMetrics(modelId) includes port",
                      gd.get("port") == launch_port, f"got {val}")
                check("getMetrics(modelId) includes llama-server PID",
                      isinstance(gd.get("pid"), int) and gd.get("pid") > 0, f"got {val}")
                check("getMetrics(modelId) includes thread count",
                      (gd.get("threads") or 0) > 0, f"got {val}")

                # getMetrics（无参）：models[] 含该模型的指标
                cid, val = await evaluate(ws, cid,
                    '(async()=>{ var r = await window.__cpp__.chat.getMetrics(); return JSON.stringify(r); })()',
                    await_promise=True, timeout_ms=5000)
                allm = json.loads(val)["data"].get("models", [])
                this_m = next((m for m in allm if m.get("modelId") == model_id), None)
                check("getMetrics() per-model list includes running model",
                      this_m is not None and (this_m.get("memoryMB") or 0) > 0, f"got {val}")

                # ---- UI: 模型列表行出现资源指标徽标 ----
                # 切到模型页并等待 service-panel 轮询把 metrics 写回 AppState
                await evaluate(ws, cid,
                    'document.querySelector(\'.tab-btn[data-tab="model"]\').click()',
                    await_promise=False, timeout_ms=2000)
                cid, val = await evaluate(ws, cid,
                    '(function(){ var m = (window.AppState.models||[]).find(function(x){return x.id==="' + model_id + '";}); if(m){ m.status="running"; } if(window.renderModels) window.renderModels(); return "ok"; })()',
                    timeout_ms=3000)
                # 等待一个轮询周期（2s）让 metrics 注入并重渲染
                cid, val = await evaluate(ws, cid, """
                    new Promise(function(resolve){
                        var tries = 0;
                        function chk(){
                            tries++;
                            var el = document.querySelector('.model-row-metrics');
                            if (el && el.textContent.indexOf('💾') >= 0) { resolve(el.textContent); }
                            else if (tries > 20) { resolve('TIMEOUT:' + (el ? el.textContent : 'no-el')); }
                            else setTimeout(chk, 300);
                        }
                        chk();
                    })
                """, await_promise=True, timeout_ms=8000)
                safe_print(f"  DIAG: row metrics text = {val}")
                check("model row shows resource metrics badge",
                      isinstance(val, str) and '💾' in val and not val.startswith('TIMEOUT'),
                      f"got {val}")

                # ---- UI: 详情页资源占用区块显示 ----
                cid, val = await evaluate(ws, cid,
                    'window.showDetail("' + model_id + '"); '
                    'document.getElementById("detailMetricsSection").style.display !== "none" ? "shown" : "hidden"',
                    timeout_ms=3000)
                check("detail page shows resource section for running model",
                      val == "shown", f"got {val}")

                # 等详情页指标拉到真实数值（主动 getMetrics(id) 或轮询刷新）
                cid, val = await evaluate(ws, cid, """
                    new Promise(function(resolve){
                        var tries = 0;
                        function chk(){
                            tries++;
                            var t = document.getElementById('detailMemVal').textContent;
                            if (t && t !== '0 MB') resolve(t);
                            else if (tries > 20) resolve('TIMEOUT:' + t);
                            else setTimeout(chk, 300);
                        }
                        chk();
                    })
                """, await_promise=True, timeout_ms=8000)
                print(f"  DIAG: detail mem text = {val}")
                check("detail page memory value populated",
                      isinstance(val, str) and not val.startswith('TIMEOUT') and val != '0 MB',
                      f"got {val}")

                # 详情页补充信息应包含 PID（即 llama-server 进程号）
                cid, val = await evaluate(ws, cid,
                    'document.getElementById("detailMetricsExtra").textContent',
                    timeout_ms=3000)
                print(f"  DIAG: detail extra text = {val}")
                check("detail page shows llama-server PID",
                      isinstance(val, str) and 'PID' in val, f"got {val}")

                # 关闭详情页
                await evaluate(ws, cid, 'document.getElementById("detailBackBtn").click()',
                    await_promise=False, timeout_ms=2000)

                # 停止该模型
                cid, val = await evaluate(ws, cid,
                    '(async()=>{ var r = await window.__cpp__.chat.stopModel({modelId:"' + model_id + '"}); return JSON.stringify(r); })()',
                    await_promise=True, timeout_ms=15000)
                print(f"  DIAG: stopModel(modelId) = {val}")
                sd = json.loads(val)["data"]
                check("stopModel(modelId) stops running model",
                      sd.get("status") == "stopped" and sd.get("modelId") == model_id, f"got {val}")

                # 停止后 getStatus(modelId) 回到 stopped
                cid, val = await evaluate(ws, cid,
                    '(async()=>{ var r = await window.__cpp__.chat.getStatus({modelId:"' + model_id + '"}); return JSON.stringify(r); })()',
                    await_promise=True, timeout_ms=5000)
                check("getStatus(modelId) reports stopped after stop",
                      json.loads(val)["data"].get("status") == "stopped", f"got {val}")
        else:
            print("  DIAG: no gguf model file found, skipping real-launch test")

        # stopModel（无参）应停止全部并返回 stopped
        cid, val = await evaluate(ws, cid,
            '(async()=>{ var r = await window.__cpp__.chat.stopModel(); return JSON.stringify(r); })()',
            await_promise=True, timeout_ms=15000)
        print(f"  DIAG: chat.stopModel(all) = {val}")
        check("chat.stopModel() stops all and returns stopped",
              json.loads(val)["data"].get("status") == "stopped", f"got {val}")

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
