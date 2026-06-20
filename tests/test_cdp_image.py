"""CDP 端到端测试：FLUX.1-schnell 真实生成图片（需 GPU + 完整模型 + llama-box-bin）"""
import asyncio, json, http.client, os, base64, sys

PORT = 9222
EXE = os.path.join(os.path.dirname(os.path.dirname(__file__)), "build", "Debug", "WebViewCpp.exe")
OUT_PNG = os.path.join(os.path.dirname(__file__), "cdp_generated_image.png")

def ws_url(port):
    c = http.client.HTTPConnection("localhost", port, timeout=5)
    c.request("GET", "/json")
    r = c.getresponse(); t = json.loads(r.read()); c.close()
    for x in t:
        if x.get("type") == "page":
            return x["webSocketDebuggerUrl"]
    raise RuntimeError("no page target")

async def ev(ws, i, e, ap=False, tm=10000):
    i += 1
    await ws.send(json.dumps({"id": i, "method": "Runtime.evaluate",
        "params": {"expression": e, "returnByValue": True, "awaitPromise": ap, "timeout": tm}}))
    dl = asyncio.get_event_loop().time() + tm//1000 + 30
    while asyncio.get_event_loop().time() < dl:
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=2)
        except asyncio.TimeoutError:
            continue
        d = json.loads(raw)
        if d.get("id") == i:
            if "exceptionDetails" in d.get("result", {}):
                return i, "EXC:" + str(d["result"]["exceptionDetails"])[:200]
            return i, d.get("result", {}).get("result", {}).get("value", "<nv>")
    return i, "TIMEOUT"

def check(name, ok, detail=""):
    print(("  [PASS] " if ok else "  [FAIL] ") + name + ("" if ok else " - " + detail))
    return ok

async def main():
    import subprocess
    subprocess.run("taskkill /f /im WebViewCpp.exe", shell=True, stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    subprocess.run("taskkill /f /im llama-box.exe", shell=True, stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    await asyncio.sleep(2)
    p = subprocess.Popen([EXE, f"--cdp-port={PORT}"], cwd=os.path.dirname(EXE),
                         creationflags=subprocess.CREATE_NO_WINDOW)
    await asyncio.sleep(5)
    ws = await __import__("websockets").connect(ws_url(PORT), max_size=2**26)
    passed = failed = 0
    try:
        i = 0
        # 1. 启动 schnell（GPU 全卸载 ngl=-1）
        print("1. 启动 FLUX.1-schnell (llama-box, GPU)...")
        i, v = await ev(ws, i, """
            (async()=>{ var r = await window.chatService.startModel({
                modelId:'FLUX.1-schnell',
                ggufPath:'downloads/FLUX.1-schnell/FLUX.1-schnell-pure-Q4_0.gguf',
                backend:'llama-box', ngl:-1, threads:4
            }); return JSON.stringify(r); })()
        """, ap=True, tm=120000)
        print("   ", str(v)[:160])
        r = json.loads(v) if v.startswith("{") else {}
        ok = r.get("ok") and r.get("data", {}).get("status") == "running"
        passed += check("startModel FLUX.1-schnell running", ok, str(v)[:120]) and 1 or (failed:=failed+1) and 0
        if not ok:
            print("   启动失败，终止"); return

        # 2. generateImage 生成图片
        print("2. 生成图片 (a cute orange cat, GPU 推理)...")
        i, v = await ev(ws, i, """
            (async()=>{
                var b64=null;
                try {
                    var r = await window.chatService.generateImage('a cute orange cat sitting on a wooden chair', function(x){b64=x;});
                    var rb = (r&&r.data&&r.data.b64_json)?r.data.b64_json:null;
                    return JSON.stringify({ok:true, cbLen: b64?b64.length:0, resLen: rb?rb.length:0});
                } catch(e){ return JSON.stringify({ok:false, err:String(e)}); }
            })()
        """, ap=True, tm=300000)  # 生成给 5 分钟
        print("   ", str(v)[:200])
        g = json.loads(v) if v.startswith("{") else {}
        gen_ok = g.get("ok") and (g.get("cbLen",0) > 1000 or g.get("resLen",0) > 1000)
        if check("generateImage returns b64 image data", gen_ok, str(v)[:150]): passed += 1
        else: failed += 1

        # 3. 取回 b64 存成 PNG 文件，验证是有效图片
        if gen_ok:
            print("3. 取回图片数据存为 PNG...")
            i, b64 = await ev(ws, i, """
                (async()=>{ var b=null;
                    await window.chatService.generateImage('a red apple on a white plate', function(x){b=x;});
                    return b; })()
            """, ap=True, tm=300000)
            if isinstance(b64, str) and len(b64) > 1000 and not b64.startswith(("EXC","TIMEOUT","<")):
                data = base64.b64decode(b64)
                with open(OUT_PNG, "wb") as f:
                    f.write(data)
                is_png = data[:8] == b'\x89PNG\r\n\x1a\n'
                if check(f"saved valid PNG ({len(data)} bytes) -> {OUT_PNG}", is_png, "not PNG header"): passed += 1
                else: failed += 1
            else:
                check("second generateImage b64", False, str(b64)[:100]); failed += 1

        # 4. 停止
        print("4. 停止模型...")
        i, v = await ev(ws, i, "(async()=>{var r=await window.chatService.stopModel('FLUX.1-schnell');return JSON.stringify(r);})()", ap=True, tm=15000)
        stopped = '"status":"stopped"' in str(v)
        if check("stopModel", stopped, str(v)[:100]): passed += 1
        else: failed += 1
    finally:
        await ws.close()
        subprocess.run("taskkill /f /im llama-box.exe", shell=True, stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        p.terminate()
    print(f"\n图片生成 CDP 测试: {passed} passed, {failed} failed")
    sys.exit(0 if failed == 0 else 1)

asyncio.run(main())
