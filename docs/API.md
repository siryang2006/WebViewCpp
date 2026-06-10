# API 参考文档

## 目录

- [CppObject 基类](#cppobject-基类)
- [WebViewWrapper 核心类](#webviewwrapper-核心类)
- [JSON 库](#json-库)
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
        bind_sync("method", [](const json::Value& args) -> json::Value { ... });
        bind_async("async_method", [](const std::string& id, const json::Value& args, WebViewWrapper* wv) { ... });
    }
    std::string object_name() const override { return "myService"; }
};
```

### 纯虚函数

| 方法 | 返回类型 | 说明 |
|------|----------|------|
| `object_name()` | `std::string` | 返回对象在 JS 端的名称，如 `"math"` 则 JS 通过 `window.__cpp__.math` 访问 |

### 生命周期钩子

| 方法 | 说明 |
|------|------|
| `on_created()` | 对象创建后自动调用，可用于初始化资源 |
| `on_destroyed()` | 对象销毁前自动调用，可用于清理资源 |

### 方法注册

#### bind_sync

```cpp
void bind_sync(const std::string& name, SyncMethod fn);
```

注册同步方法。JS 调用后立即返回结果。

**参数:**
- `name` — 方法名
- `fn` — `std::function<json::Value(const json::Value&)>`

**示例:**
```cpp
bind_sync("add", [](const json::Value& args) -> json::Value {
    return args[0].to_int() + args[1].to_int();
});
```

**JS 调用:**
```javascript
const result = window.__cpp__.math.add(1, 2);
```

#### bind_async

```cpp
void bind_async(const std::string& name, AsyncMethod fn);
```

注册异步方法。JS 调用后返回 Promise，通过 `resolve/reject` 返回结果。

**参数:**
- `name` — 方法名
- `fn` — `std::function<void(const std::string& id, const json::Value& args, WebViewWrapper* wv)>`

**回调参数:**
- `id` — 唯一调用标识，用于 resolve/reject
- `args` — JS 传入的参数数组
- `wv` — WebViewWrapper 指针，用于调用 resolve/reject

**示例:**
```cpp
bind_async("fetch", [](const std::string& id, const json::Value& args, WebViewWrapper* wv) {
    std::string url = args[0].to_string();
    std::thread([id, url, wv]() {
        auto data = http_get(url);
        wv->resolve(id, json::Value(data));
    }).detach();
});
```

**JS 调用:**
```javascript
try {
    const data = await window.__cpp__.service.fetch("https://api.example.com");
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

#### bind_property_set (读写)

```cpp
template<typename Setter>
void bind_property_set(const std::string& name, Setter set);
```

**示例:**
```cpp
bind_property("count", [this]() -> int { return m_count; });
bind_property_set("count", [this](const json::Value& val) { m_count = val.to_int(); });
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
| `has_sync_method(name)` | 是否有同步方法 |
| `has_async_method(name)` | 是否有异步方法 |

---

## WebViewWrapper 核心类

### 头文件

```cpp
#include "WebViewWrapper.h"
```

### 构造 / 析构

```cpp
WebViewWrapper();
~WebViewWrapper();
```

### 初始化

```cpp
bool init(const std::string& title, const std::string& url = "",
          int width = 800, int height = 600, bool resizable = true);
```

| 参数 | 说明 |
|------|------|
| `title` | 窗口标题 |
| `url` | 初始 URL (空则不导航) |
| `width` | 窗口宽度 |
| `height` | 窗口高度 |
| `resizable` | 是否可调整大小 |

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
```

### JavaScript 执行

```cpp
void eval(const std::string& js);  // 执行 JS 代码
```

### 对象绑定

```cpp
void bind_object(std::shared_ptr<CppObject> obj);
```

将 C++ 对象注册到 JS 端。注册后 JS 可通过 `window.__cpp__.对象名.方法名()` 调用。

### 回调机制

#### resolve

```cpp
void resolve(const std::string& id, const json::Value& result);
```

异步方法完成时调用，通知 JS 成功结果。

#### reject

```cpp
void reject(const std::string& id, const std::string& error);
```

异步方法失败时调用，通知 JS 错误信息。

#### call_js

```cpp
void call_js(const std::string& func_name,
             const json::Value& args,
             std::function<void(const json::Value&)> on_result = nullptr,
             std::function<void(const std::string&)> on_error = nullptr);
```

C++ 调用 JS 函数。支持 JS 函数返回 Promise。

#### call_registered_js

```cpp
void call_registered_js(const std::string& name, const json::Value& args);
```

调用 JS 端通过 `__register_cb__` 注册的回调函数。

#### eval_js

```cpp
void eval_js(const std::string& js_code);
```

执行 JS 代码 (等同于 `eval()`)。

---

## JSON 库

### 头文件

```cpp
#include "binding/json.h"
```

### Value 类型

```cpp
json::Value v;           // null
json::Value v(42);       // number
json::Value v(3.14);     // number
json::Value v(true);     // bool
json::Value v("hello");  // string

json::Value arr;         // array
arr.push(1);
arr.push("two");

json::Value obj;         // object
obj["key"] = "value";
obj["nested"]["deep"] = 42;
```

### 方法

| 方法 | 返回类型 | 说明 |
|------|----------|------|
| `type()` | `Type` | 获取类型 (Null/Bool/Number/String/Array/Object) |
| `is_null()` | `bool` | 是否为 null |
| `to_bool()` | `bool` | 转为 bool |
| `to_int()` | `int` | 转为 int |
| `to_number()` | `double` | 转为 double |
| `to_string()` | `const string&` | 获取字符串 |
| `dump()` | `string` | 序列化为 JSON 字符串 |
| `push(v)` | `void` | 添加到数组 |
| `size()` | `size_t` | 数组/对象大小 |
| `has(key)` | `bool` | 对象是否包含 key |

### 解析

```cpp
json::Value v = json::parse(R"({"name":"Alice","age":30})");
std::string name = v["name"].to_string();
int age = v["age"].to_int();
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
    USER_ERROR         = -100,  // 用户自定义错误起始值
};
```

### 异常类

```cpp
class BindingException : public std::runtime_error {
public:
    ErrorCode code() const;
};
```

### 错误结果

```cpp
ErrorResult err(ErrorCode::METHOD_NOT_FOUND, "method 'foo' not found");
json::Value j = err.to_json();
// {"ok":false, "code":-1, "error":"method 'foo' not found"}
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
| `__cpp_result__(id, success, valueJson)` | C++ 调用 JS 后的回调 |
| `__register_cb__(name, fn)` | JS 注册回调供 C++ 调用 |

### 调用流程

#### JS → C++ 同步调用

```
JS: window.__cpp__.math.add(1, 2)
  ↓
JS Bridge: __webview_sync_call__("math", "add", "[1,2]")
  ↓
C++: CppObject::invoke_sync("add", [1,2])
  ↓
返回: 3
```

#### JS → C++ 异步调用

```
JS: await window.__cpp__.math.slow_add(1, 2)
  ↓
JS Bridge: __webview_async_call__("math", "slow_add", {"args":[1,2],"id":"__cb_xxx"})
  ↓
C++: CppObject::invoke_async("slow_add", "__cb_xxx", [1,2], wv)
  ↓ (后台线程)
C++: wv->resolve("__cb_xxx", 3)
  ↓
JS: Promise resolves with 3
```

#### C++ → JS 调用

```
C++: wv.call_registered_js("onUpdate", json::Value("data"))
  ↓
JS: window.onUpdate("data")
```
