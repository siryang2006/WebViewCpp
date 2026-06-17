# API 参考文档

## 目录

- [CppObject 基类](#cppobject-基类)
- [WebViewWrapper 核心类](#webviewwrapper-核心类)
- [类型安全绑定](#类型安全绑定)
- [工厂模式](#工厂模式)
- [错误处理](#错误处理)
- [JS Bridge 协议](#js-bridge-协议)

---

## CppObject 基类

`CppObject` 是所有暴露给 JS 的 C++ 对象的基类。

### 头文件

```cpp
#include "binding/CppObject.h"
```

### 继承方式

```cpp
class MyService : public CppObject {
public:
    MyService() {
        // 类型安全模式
        bind_sync("add", [](int a, int b) { return a + b; });
        bind_sync("greet", [](const std::string& name) { return "Hello " + name; });

        // 类型擦除模式
        bind_sync("raw", [](const json& args) { return args[0].get<int>() + 1; });

        // 异步方法
        bind_async("fetch", [](const std::string& id, const json& args, WebViewWrapper* wv) {
            wv->dispatch_task([id, wv]() {
                wv->resolve(id, "ok");
            });
        });
    }
    std::string object_name() const override { return "myService"; }
};
```

### 纯虚函数

| 方法 | 返回类型 | 说明 |
|------|----------|------|
| `object_name()` | `std::string` | 对象在 JS 端的名称，如 `"math"` 则 JS 通过 `window.__cpp__.math` 访问 |

### 生命周期钩子

| 方法 | 说明 |
|------|------|
| `on_created()` | 对象创建后自动调用，可用于初始化资源 |
| `on_destroyed()` | 对象销毁前自动调用，可用于清理资源 |

### 方法注册

#### bind_sync

```cpp
template<typename Fn>
void bind_sync(const std::string& name, Fn fn);
```

注册同步方法。JS 调用后立即返回结果。

**类型安全模式（推荐）:**

```cpp
bind_sync("add", [](int a, int b) { return a + b; });
bind_sync("greet", [](const std::string& name) { return "Hello " + name; });
bind_sync("log", [](double value, const std::string& msg) { std::cout << msg << value; });
```

**类型擦除模式:**

```cpp
bind_sync("raw", [](const json& args) {
    return args[0].get<int>() + args[1].get<int>();
});
```

**JS 调用:**

```javascript
const result = window.__cpp__.math.add(10, 20);  // 30
```

#### bind_async

```cpp
template<typename Fn>
void bind_async(const std::string& name, Fn fn);
```

注册异步方法。JS 调用后返回 Promise，通过 `resolve/reject` 返回结果。

**参数:**
- `id` — 唯一调用标识，用于 resolve/reject
- `args` — JS 传入的参数（JSON 数组）
- `wv` — WebViewWrapper 指针

**示例:**

```cpp
bind_async("fetch", [](const std::string& id, const json& args, WebViewWrapper* wv) {
    std::string url = args[0].get<std::string>();
    wv->dispatch_task([id, url, wv]() {
        auto data = http_get(url);
        wv->resolve(id, {{"status", 200}, {"data", data}});
        // 或 wv->reject(id, "network error");
    });
});
```

**JS 调用:**

```javascript
try {
    const data = await window.__cpp__.service.fetch("https://api.example.com");
    console.log(data.status);  // 200
} catch (e) {
    console.error(e.message);
}
```

### 属性注册

#### bind_property (只读)

```cpp
template<typename Getter>
void bind_property(const std::string& name, Getter get);
```

**示例:**

```cpp
bind_property("version", []() -> std::string { return "1.0"; });
bind_property("pi", []() -> double { return 3.14159; });
```

#### bind_property (读写)

```cpp
template<typename Getter, typename Setter>
void bind_property(const std::string& name, Getter get, Setter set);
```

**示例:**

```cpp
bind_property("count",
    [this]() -> int { return m_count; },
    [this](int v) { m_count = v; }
);
```

**JS 调用:**

```javascript
console.log(window.__cpp__.obj.count);  // 读取
window.__cpp__.obj.count = 42;          // 设置
```

### 辅助方法

| 方法 | 说明 |
|------|------|
| `ok_result(data)` | 创建成功结果 `{"ok":true, "data":...}` |
| `error_result(code, msg)` | 创建错误结果 `{"ok":false, "code":-1, "error":"..."}` |
| `instance_id()` | 返回唯一实例 ID |
| `is_destroyed()` | 对象是否已销毁 |
| `js_escape(s)` | 转义字符串用于 JS 注入 |

---

## WebViewWrapper 核心类

### 头文件

```cpp
#include "WebViewWrapper.h"
```

### 初始化

```cpp
bool init(const std::string& title, const std::string& url = "",
          int width = 800, int height = 600, bool resizable = true,
          int debug_port = 0);
```

| 参数 | 说明 |
|------|------|
| `title` | 窗口标题 |
| `url` | 初始 URL (空则不导航) |
| `width` | 窗口宽度 |
| `height` | 窗口高度 |
| `resizable` | 是否可调整大小 |
| `debug_port` | CDP 远程调试端口 (0=禁用, >0=端口号, 仅 Windows 有效) |

### 主循环

```cpp
void run();       // 阻塞运行主循环
void terminate(); // 终止主循环
```

### 导航

```cpp
void navigate(const std::string& url);    // 导航到 URL
void set_html(const std::string& html);   // 直接设置 HTML 内容
void set_title(const std::string& title); // 设置窗口标题
void eval(const std::string& js);         // 执行 JS 代码
```

### 对象绑定

```cpp
void bind_object(std::shared_ptr<CppObject> obj);
void unbind_object(const std::string& name);
```

将 C++ 对象注册到 JS 端。注册后 JS 可通过 `window.__cpp__.对象名.方法名()` 调用。

### 工厂模式

```cpp
enum class FactoryMode { Instance, Global };

void bind_factory(const std::string& type_name, ObjectFactory factory,
                  FactoryMode mode = FactoryMode::Instance);
```

**Instance 模式** — JS `new` 创建 C++ 实例，GC 自动清理：

```cpp
wv.bind_factory("Worker", WorkerService::create);
// JS: new window.__cpp__.Worker("Alice", 5)
```

**Global 模式** — JS 直接调用，C++ 使用全局单例：

```cpp
wv.bind_factory("System", SystemService::create, WebViewWrapper::FactoryMode::Global);
// JS: window.__cpp__.System.getStatus()
```

### 回调机制

```cpp
void resolve(const std::string& id, const json& result);
void reject(const std::string& id, const std::string& error);
```

异步方法完成时调用。

```cpp
void call_js(const std::string& func_name, const json& args,
             std::function<void(const json&)> on_result = nullptr,
             std::function<void(const std::string&)> on_error = nullptr,
             int timeout_ms = 10000);
```

C++ 调用 JS 函数，支持 Promise 返回值。

```cpp
void call_registered_js(const std::string& name, const json& args);
```

调用 JS 端通过 `__register_cb__` 注册的回调。

### 任务派发

```cpp
void dispatch_task(std::function<void()> fn);
```

线程安全的任务派发。替代 `std::thread::detach`，WebView 销毁时自动取消未执行任务。

### 状态查询

```cpp
bool is_ready() const;  // WebView 是否就绪且未终止
```

---

## 类型安全绑定

### 支持类型

| C++ 类型 | JS 类型 | 说明 |
|----------|---------|------|
| `int` | number | 整数 |
| `double` | number | 浮点数 |
| `bool` | boolean | 布尔值 |
| `std::string` | string | 字符串 |
| `json` | any | 任意 JSON 值 |
| `void` (返回值) | undefined | 无返回值 |

### 两种模式

```cpp
// 1. 类型安全 — 编译期检查 + 详细运行时错误
bind_sync("add", [](int a, int b) { return a + b; });

// 2. 类型擦除 — 手动解析 JSON
bind_sync("raw", [](const json& args) {
    return args[0].get<int>() + args[1].get<int>();
});
```

检测逻辑：lambda 第一个参数是 `const json&` → 类型擦除；否则 → 类型安全。

### 编译期静态断言

| 检查项 | 禁止 | 错误 |
|--------|------|------|
| 指针类型 | `int*` | `pointer types not allowed` |
| void 参数 | `void` | `void not allowed as argument` |
| 引用类型 | `int&` | `reference types not allowed` |

### 运行时错误信息

```cpp
bind_sync("add", [](int a, int b) { return a + b; });
```

| 调用 | 错误信息 |
|------|---------|
| `add(1)` | `[add] Argument count mismatch: expected 2, got 1` |
| `add("x", 2)` | `[add] Argument 0 type mismatch: expected int, got string` |
| `add(null, 2)` | `[add] Argument 0 type mismatch: expected int, got null` |

---

## 工厂模式

### 注册

```cpp
class WorkerService : public CppObject {
public:
    WorkerService(const std::string& name, int priority)
        : m_name(name), m_priority(priority) {
        bind_sync("getName", [this](const json&) { return m_name; });
    }

    static std::shared_ptr<WorkerService> create(const json& args) {
        return std::make_shared<WorkerService>(
            args[0].get<std::string>(),
            args[1].get<int>()
        );
    }

    std::string object_name() const override { return "worker_" + m_name; }
    void on_destroyed() override { /* 清理资源 */ }

private:
    std::string m_name;
    int m_priority;
};

wv.bind_factory("Worker", WorkerService::create);
```

### JS 使用

```javascript
const worker = new window.__cpp__.Worker("Alice", 5);
console.log(worker.getName());  // "Alice"

// 手动销毁
worker.__destroy__();

// 或等待 JS GC 自动清理（FinalizationRegistry）
worker = null;
```

### 生命周期同步

```
JS 创建 new Worker() → C++ make_shared<WorkerService>()
       ↓
JS proxy 持有实例 ID
       ↓
FinalizationRegistry.register(proxy, instanceId)
       ↓
JS GC 回收 proxy → 触发 callback → __webview_destroy__(instanceId)
       ↓
C++ 从 map 移除 → shared_ptr 引用归零 → ~WorkerService()
```

---

## 错误处理

### 错误码

```cpp
enum class ErrorCode {
    OK                 = 0,
    METHOD_NOT_FOUND   = -1,
    PROPERTY_NOT_FOUND = -2,
    INVALID_ARGUMENTS  = -3,
    OBJECT_DESTROYED   = -4,
    INTERNAL_ERROR     = -5,
    USER_ERROR         = -100,
};
```

### 异常类

```cpp
class BindingException : public std::runtime_error {
public:
    ErrorCode code() const;
};
```

### 使用

```cpp
// 抛出异常自动传递到 JS
bind_sync("risky", []() {
    throw BindingException(ErrorCode::USER_ERROR, "something went wrong");
});
```

JS 端：

```javascript
try {
    window.__cpp__.svc.risky();
} catch (e) {
    console.error(e.message);  // "something went wrong"
}
```

---

## JS Bridge 协议

### 内部桥接函数

| JS 函数 | 说明 |
|---------|------|
| `__webview_sync_call__(obj, method, argsJson)` | 同步调用 C++ 方法 |
| `__webview_async_call__(obj, method, reqJson)` | 异步调用 C++ 方法 |
| `__webview_get_property__(obj, prop)` | 读取 C++ 属性 |
| `__webview_set_property__(obj, prop, valJson)` | 设置 C++ 属性 |
| `__webview_create__(typeName, argsJson)` | 工厂创建实例 |
| `__webview_destroy__(instanceId)` | 销毁实例 |
| `__webview_list_types__()` | 列出已注册的工厂类型 |
| `__cpp_result__(id, success, valueJson)` | C++ 调用 JS 回调 |
| `__register_cb__(name, fn)` | JS 注册命名回调供 C++ 调用 |
| `__store_js_fn__(id, fn)` | 暂存匿名 JS 回调函数（按 id），供 C++ 触发 |
| `__call_js_fn__(id, args)` | 调用 `__js_callbacks__[id]` 存储的 JS 函数 |
| `__delete_js_fn__(id)` | 删除已暂存的 JS 回调 |

### 内部状态对象

| JS 全局 | 说明 |
|---------|------|
| `window.__cpp__` | 所有 C++ 对象的根命名空间 |
| `window.__pending_callbacks__` | id → `{resolve, reject}`，Promise 型异步调用的待决回调 |
| `window.__js_callbacks__` | id → `function`，回调风格传入的匿名 JS 函数 |
| `window.__registered_cbs__` | name → `function`，`__register_cb__` 注册的命名回调 |

### 调用流程

#### JS → C++ 同步

```
JS: window.__cpp__.math.add(10, 20)
  ↓
JS Bridge: __webview_sync_call__("math", "add", "[10,20]")
  ↓
C++: CppObject::invoke_sync("add", [10, 20])
  ↓ (类型安全: extract_one<int>, extract_one<int>)
返回: 30
```

#### JS → C++ 异步

```
JS: await window.__cpp__.svc.fetch("url")
  ↓
JS Bridge: __webview_async_call__("svc", "fetch", {"args":["url"],"id":"__cb_xxx"})
  ↓
C++: CppObject::invoke_async("fetch", "__cb_xxx", ["url"], wv)
  ↓ (后台 dispatch_task)
C++: wv->resolve("__cb_xxx", {"status": 200})
  ↓
JS: Promise resolves
```

#### JS new → C++ 创建

```
JS: new window.__cpp__.Worker("Alice", 5)
  ↓
JS: __webview_create__("Worker", "[\"Alice\",5]")
  ↓
C++: factory(args) → make_shared<WorkerService>("Alice", 5)
  ↓
C++: m_dynamic_instances[id] = obj
  ↓
JS: FinalizationRegistry.register(proxy, id)
返回: Proxy { __id__: "Worker_1", ... }
```

#### C++ → JS

```
C++: wv.call_registered_js("onUpdate", {"event": "click"})
  ↓
JS: window.__registered_cbs__["onUpdate"]({"event": "click"})
```

---

## 异步回调实现原理

`bind_async` 的方法在 JS 端支持两种调用形态，二者共用同一套 C++ 后端：

```javascript
// 形态 A：Promise
const r = await window.__cpp__.math.slow_add(1, 2);

// 形态 B：回调风格（末尾传一个函数，Node 约定 cb(err, result)）
window.__cpp__.math.slow_add(1, 2, function(err, result) { ... });
```

核心设计：**JS 函数永远不跨越 JS↔C++ 边界**。JS 函数无法被 JSON
序列化，因此 C++ 侧从不持有函数本身，只持有一个字符串 `id`；真正的回调动作
由 C++ 通过 `eval` 注入一段 JS 代码、在 JS 侧查表执行来完成。

### 完整链路（以形态 B 为例）

#### 第 1 步：JS 包装函数识别回调并暂存

`inject_single_object` 为每个 async 方法注入的包装函数：

```javascript
window.__cpp__.math.slow_add = function() {
    var args = Array.prototype.slice.call(arguments);   // [1, 2, fn]
    var cb_fn = null;
    // 末尾参数若为函数，弹出作为回调
    if (typeof args[args.length-1] === 'function') { cb_fn = args.pop(); }
    return new Promise(function(resolve, reject) {
        var id = window.__next_cb_id__();               // 生成唯一 id
        // 回调风格：登记 null（占位）；Promise 风格：登记 {resolve, reject}
        window.__pending_callbacks__[id] = cb_fn ? null : { resolve, reject };
        if (cb_fn) { window.__store_js_fn__(id, cb_fn); } // 函数存进 __js_callbacks__[id]
        var req = JSON.stringify({ args: args, id: id, has_cb: !!cb_fn });
        window.__webview_async_call__('math', 'slow_add', req);
    });
};
```

要点：
- 传给 C++ 的 `req` 只含 `args`（已剔除函数）、`id`、`has_cb` 布尔标记。
- JS 函数被 `__store_js_fn__` 存入 `window.__js_callbacks__[id]`，留在 JS 侧。
- 即使是回调风格，包装函数仍返回 Promise，所以两种形态可混用。

#### 第 2 步：C++ 登记回调类型并派发

`__webview_async_call__` 绑定（`setup_js_bridge`）：

```cpp
bool has_cb = call_data.value("has_cb", false);
// 仅记录该 id 属于「JS 函数型」还是「Promise 型」，不持有任何函数
self->m_pending_callbacks[id] = { nullptr, nullptr, /*is_js_callback=*/has_cb };
// 在派发线程执行用户 lambda，避免阻塞 GUI 线程
self->dispatch_task([obj, method, id, args, self]() {
    obj->invoke_async(method, id, args, self);
});
```

`PendingCallback` 结构：

```cpp
struct PendingCallback {
    std::function<void(const json&)>        on_result;     // call_js 等场景用
    std::function<void(const std::string&)> on_error;
    bool is_js_callback = false;   // true → 回调风格；false → Promise 风格
};
```

#### 第 3 步：用户 lambda 调用 resolve / reject

```cpp
bind_async("slow_add", [](const std::string& id, const json& args, WebViewWrapper* wv) {
    int a = args[0], b = args[1];
    wv->dispatch_task([id, a, b, wv]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        wv->resolve(id, a + b);          // 成功
        // wv->reject(id, "some error"); // 失败
    });
});
```

#### 第 4 步：resolve / reject 按类型注入 JS

`WebViewWrapper::resolve` 根据 `is_js_callback` 分派：

```cpp
void WebViewWrapper::resolve(const std::string& id, const json& result) {
    bool is_js_cb;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        auto it = m_pending_callbacks.find(id);
        if (it == m_pending_callbacks.end()) return;   // 已消费/超时
        is_js_cb = it->second.is_js_callback;
        m_pending_callbacks.erase(it);
    }
    std::string json_str = result.dump(-1, ' ', /*ensure_ascii=*/true);
    std::string js;
    if (is_js_cb) {
        // 回调风格：Node 约定 fn(null, result)，调用后删除
        js = "var fn=window.__js_callbacks__['" + id + "'];"
             "if(typeof fn==='function'){fn(null," + json_str + ");"
             "delete window.__js_callbacks__['" + id + "'];}";
    } else {
        // Promise 风格：取出 {resolve,reject} 并 resolve
        js = "var cb=window.__pending_callbacks__['" + id + "'];"
             "if(cb){cb.resolve(" + json_str + ");"
             "delete window.__pending_callbacks__['" + id + "'];}";
    }
    post_eval(js);   // 注入 JS，在 GUI 线程消息循环中执行
}
```

`reject` 对称处理：回调风格走 `fn(error, null)`，Promise 风格走 `cb.reject(new Error(...))`。

### 时序总览

```
JS                          C++
──                          ───
slow_add(1,2,fn)
  pop fn → __js_callbacks__[id]=fn
  __webview_async_call__ ──────► 登记 {is_js_callback:true}
                                 dispatch_task → 用户 lambda（后台线程）
                                   ... 2s ...
                                 wv->resolve(id, 3)
  __js_callbacks__[id](null,3) ◄── post_eval 注入 JS
  delete __js_callbacks__[id]
```

### 设计要点

| 关注点 | 处理方式 |
|--------|---------|
| JS 函数不可序列化 | 函数留在 JS 侧 `__js_callbacks__`，C++ 只持有 `id` |
| 两种调用形态统一 | 包装函数恒返回 Promise，`has_cb` 决定回调路径 |
| 线程安全 | 用户 lambda 在派发线程执行；`m_callback_mutex` 保护待决表 |
| 跨线程回 JS | `resolve/reject` 用 `post_eval`，由 GUI 线程消息循环执行 |
| 防重复/超时 | 查表后立即 `erase`；`call_js` 类带 `timeout_ms` 超时清理 |
| 非 ASCII 安全 | `dump(-1, ' ', true)` 转义 U+2028/U+2029 等字符 |

### 与命名回调（`__register_cb__`）的区别

| | 匿名回调（`__store_js_fn__`） | 命名回调（`__register_cb__`） |
|---|---|---|
| 触发方 | C++ 完成 async 调用后自动回调 | C++ 主动 `call_registered_js(name, ...)` |
| 标识 | 一次性 `id`，调用后删除 | 字符串 `name`，可反复触发 |
| 存储 | `window.__js_callbacks__[id]` | `window.__registered_cbs__[name]` |
| 典型场景 | `fn(a,b,cb)` 异步结果回调 | C++ 主动推送事件给 JS |

---

## DownloadService 下载服务

`DownloadService` 提供模型文件下载功能，支持断点续传、暂停、继续。

### 文件

- `src/DownloadService.h` - C++ 头文件
- `src/DownloadService.cpp` - C++ 实现
- `src/DownloadService.js` - JS 封装类

### 绑定名称

C++: `window.__cpp__.download`
JS: `window.downloadService`

### 方法对照

| 方法 | C++ 类型 | JS 调用 | 说明 |
|------|----------|---------|------|
| `startDownload` | async | `startDownload(params, callback)` | 开始下载 |
| `pauseDownload` | sync | `pauseDownload(modelId)` | 暂停下载 |
| `resumeDownload` | sync | `resumeDownload(modelId)` | 继续下载 |
| `cancelDownload` | sync | `cancelDownload(modelId)` | 取消下载 |
| `getProgress` | sync | `getProgress(modelId)` | 获取进度 |
| `getSpeed` | sync | `getSpeed(modelId)` | 获取速度 |

### startDownload

**C++ 签名:**
```cpp
void startDownload(const std::string& id, const json& args, WebViewWrapper* wv);
```

**参数 (JSON):**
```json
{
    "url": "https://modelscope.cn/api/v1/models/...",
    "savePath": "downloads/gemma-4-2b/model.gguf",
    "modelId": "gemma-4-2b",
    "totalSize": 2254857830,
    "callback": "onDownloadProgress"
}
```

**JS 调用:**
```javascript
window.downloadService.startDownload({
    url: 'https://modelscope.cn/api/v1/models/...',
    savePath: 'downloads/gemma-4-2b/model.gguf',
    modelId: 'gemma-4-2b',
    totalSize: 2254857830
}, function(data) {
    console.log(data.percentage, data.status);
});
```

**进度回调数据:**
```json
{
    "modelId": "gemma-4-2b",
    "downloaded": 123456789,
    "total": 2254857830,
    "percentage": 5.47,
    "speed": 10485760,
    "status": "downloading"
}
```

**status 状态值:**
- `downloading` - 下载中
- `paused` - 已暂停
- `completed` - 下载完成
- `cancelled` - 已取消
- `error` - 下载错误

### pauseDownload

暂停指定模型的下载任务。

**JS 调用:**
```javascript
const result = await window.downloadService.pauseDownload('gemma-4-2b');
// result: {ok: true, data: {status: "paused", modelId: "gemma-4-2b"}}
```

### resumeDownload

继续指定模型的下载任务。

**JS 调用:**
```javascript
const result = await window.downloadService.resumeDownload('gemma-4-2b');
// result: {ok: true, data: {status: "resumed", modelId: "gemma-4-2b"}}
```

### cancelDownload

取消指定模型的下载任务。

**JS 调用:**
```javascript
const result = await window.downloadService.cancelDownload('gemma-4-2b');
// result: {ok: true, data: {status: "cancelled", modelId: "gemma-4-2b"}}
```

### getProgress

获取下载进度。

**JS 调用:**
```javascript
const result = await window.downloadService.getProgress('gemma-4-2b');
// result: {ok: true, data: {downloaded: 123456789, total: 2254857830, percentage: 5.47, status: "downloading"}}
```

### getSpeed

获取下载速度（字节/秒）。

**JS 调用:**
```javascript
const result = await window.downloadService.getSpeed('gemma-4-2b');
// result: {ok: true, data: {speed: 10485760}}
```

### models.json 配置

```json
{
    "id": "gemma-4-2b",
    "name": "Gemma 4 E2B",
    "size": "2.1 GB",
    "size_bytes": 2254857830,
    "status": "available",
    "download_url": "https://modelscope.cn/api/v1/models/LLM-Research/gemma-4-2b-gguf/resolve/main/gemma-4-2b-Q4_K_M.gguf"
}
```

### 使用示例

```javascript
const dl = window.downloadService;

// 下载模型
function startDownload(id) {
    const m = allModels.find(x => x.id === id);
    if (!m || !m.download_url) return;
    
    m.status = 'downloading';
    m.progress = 0;
    renderModels();
    
    dl.startDownload({
        url: m.download_url,
        savePath: `downloads/${id}/${m.download_url.split('/').pop()}`,
        modelId: id,
        totalSize: m.size_bytes
    }, function(data) {
        m.progress = data.percentage;
        if (data.status === 'completed') {
            m.status = 'downloaded';
        }
        renderModels();
    });
}

// 暂停下载
function pauseDownload(id) {
    dl.pauseDownload(id);
}

// 继续下载
function resumeDownload(id) {
    dl.resumeDownload(id);
}

// 取消下载
function cancelDownload(id) {
    dl.cancelDownload(id);
}
```
