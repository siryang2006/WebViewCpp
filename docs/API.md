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
| `__register_cb__(name, fn)` | JS 注册回调供 C++ 调用 |

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
