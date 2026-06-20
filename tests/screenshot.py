import asyncio, json, http.client
def ws(port):
    c=http.client.HTTPConnection('localhost',port,timeout=5);c.request('GET','/json')
    r=c.getresponse();t=json.loads(r.read());c.close()
    for x in t:
        if x.get('type')=='page':return x['webSocketDebuggerUrl']
async def ev(ws_c,i,e,ap=False):
    i+=1
    await ws_c.send(json.dumps({'id':i,'method':'Runtime.evaluate','params':{'expression':e,'returnByValue':True,'awaitPromise':ap,'timeout':10000}}))
    for _ in range(20):
        try:
            raw=await asyncio.wait_for(ws_c.recv(),timeout=1)
            d=json.loads(raw)
            if d.get('id')==i:return i,d.get('result',{}).get('result',{}).get('value','<nv>')
        except:continue
    return i,'<timeout>'
async def main():
    import websockets
    async with websockets.connect(ws(9222),max_size=2**20) as ws_c:
        i=0
        # basic checks
        for info in [
            'document.body.children.length',
            'document.getElementById("modelPage") !== null',
            'document.querySelectorAll(".model-page").length',
            'document.querySelectorAll("[id]").filter(function(el){return el.id.includes("model")}).map(function(el){return el.id;}).join(",")'
        ]:
            i,r=await ev(ws_c,i,'(function(){return '+info+';})()')
            print(info[:60]+':', r)

        # check body children
        i,r=await ev(ws_c,i,
            '(function(){'
            'var names=[];'
            'for(var c=document.body.firstChild;c;c=c.nextSibling){'
            '  names.push(c.tagName+(c.id?"#"+c.id:""));'
            '}'
            'return names.join(" | ");'
            '})()')
        print('body children:', str(r)[:200])
asyncio.run(main())
