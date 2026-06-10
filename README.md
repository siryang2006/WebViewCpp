# WebView C++ / JS Object Binding

基于 [webview](https://github.com/webview/webview) 库的 C++ / JavaScript 对象绑定框架，支持同步调用、异步回调、属性绑定、生命周期管理和统一错误处理。

## 架构

```
┌─────────────────────────────────────────────────┐
│              Your Application                    │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────┐ │
│  │ MathService │  │ FileService │  │  ...    │ │
│  └──────┬──────┘  └──────┬──────┘  └────┬────┘ │
│         │                │              │        │
│  ┌──────┴────────────────┴──────────────┴────┐  │
│  │            CppObject (基类)               │  │
│  │  - bind_sync()    同步方法注册            │  │
│  │  - bind_async()   异步方法注册            │  │
│  │  - bind_property() 属性注册              │  │
│  │  - 生命周期钩子                          │  │
│  │  - 统一错误处理                          │  │
│  └──────────────────┬───────────────────────┘  │
│                     │                           │
│  ┌──────────────────┴───────────────────────┐  │
│  │         WebViewWrapper (核心封装)         │  │
│  │  - bind_object()   注册对象到 JS          │  │
│  │  - resolve()       异步回调成功           │  │
│  │  - reject()        异步回调失败           │  │
│  │  - call_js()       C++ 调用 JS           │  │
│  │  - eval()          执行 JS 代码          │  │
│  └──────────────────┬───────────────────────┘  │
│                     │                           │
│  ┌──────────────────┴───────────────────────┐  │
│  │         JS Bridge (自动注入)              │  │
│  │  window.__cpp__.objectName.method()       │  │
│  │  window.__register_cb__()                 │  │
│  └──────────────────┬───────────────────────┘  │
└─────────────────────┼───────────────────────────┘
                      │
┌─────────────────────┴───────────────────────────┐
│              webview (底层引擎)                   │
│  Windows: Edge WebView2  |  Linux/macOS: WebKit  │
└─────────────────────────────────────────────────┘
```

## 项目结构

```
WebViewCpp/
├── CMakeLists.txt              # CMake 构建文件
├── build.bat                   # Windows 构建脚本
├── include/
│   ├── WebViewWrapper.h        # 核心封装类
│   └── binding/
│       ├── CppObject.h         # 基类 (绑定、错误、生命周期)
│       └── json.h              # 轻量 JSON 库
├── src/
│   ├── WebViewWrapper.cpp      # 封装实现
│   └── main.cpp                # 示例程序
├── third_party/
│   └── webview.h               # webview 库 (需手动下载)
└── web/                        # 前端资源目录
```

## 快速开始

### 1. 下载依赖

```bash
# 下载 webview.h (单头文件)
curl -L -o third_party/webview.h \
  https://raw.githubusercontent.com/webview/webview/master/core/webview.h
```

### 2. 构建

```bash
# Windows (Visual Studio)
build.bat

# 或手动 CMake
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### 3. 运行

```bash
build\Release\WebViewCpp.exe
```

## 核心用法

### 定义 C++ 对象 (继承 CppObject)

```cpp
class UserService : public CppObject {
public:
    UserService() {
        // 同步方法
        bind_sync("get_name", [](const json::Value& args) -> json::Value {
            return json::Value("Alice");
        });

        // 异步方法 (通过回调返回)
        bind_async("fetch_data", [](const std::string& id,
                                     const json::Value& args,
                                     WebViewWrapper* wv) {
            std::string url = args[0].to_string();
            // 后台线程完成后回调
            std::thread([id, url, wv]() {
                auto data = http_get(url);
                wv->resolve(id, json::Value(data));  // 成功
                // 或 wv->reject(id, "network error");  // 失败
            }).detach();
        });

        // 只读属性
        bind_property("version", []() -> std::string { return "2.0"; });
    }

    std::string object_name() const override { return "user"; }
};
```

### 注册到 WebView

```cpp
WebViewWrapper wv;
wv.init("My App", "", 800, 600);

// 注册对象
wv.bind_object(std::make_shared<UserService>());

wv.run();
```

### JS 端调用

```javascript
// 同步调用
const name = window.__cpp__.user.get_name();
console.log(name);  // "Alice"

// 异步调用 (返回 Promise)
try {
    const data = await window.__cpp__.user.fetch_data("https://api.example.com");
    console.log(data);
} catch (e) {
    console.error(e.message);
}

// 读取属性
const ver = window.__cpp__.user.version;
```

### C++ 调用 JS

```cpp
// 方式 1: 注册 JS 回调函数
// JS 端: window.__register_cb__("onUpdate", (data) => { ... });
wv.call_registered_js("onUpdate", json::Value("hello"));

// 方式 2: 直接执行 JS
wv.eval("document.title = 'Updated'");

// 方式 3: 调用 JS 函数并获取返回值
wv.call_js("myFunc", json::Value(42),
    [](const json::Value& result) { /* 成功 */ },
    [](const std::string& error) { /* 失败 */ }
);
```

## 错误处理

```cpp
// C++ 方法内抛出异常，自动传递到 JS
bind_async("risky_op", [](const std::string& id, const json::Value&, WebViewWrapper* wv) {
    try {
        auto result = do_something();
        wv->resolve(id, result);
    } catch (const std::exception& e) {
        wv->reject(id, e.what());  // JS 端 catch 捕获
    }
});
```

JS 端:
```javascript
try {
    await window.__cpp__.service.risky_op();
} catch (e) {
    console.error(e.message);  // 显示错误信息
}
```

### 错误码

| 错误码 | 值 | 含义 |
|--------|------|------|
| `OK` | 0 | 成功 |
| `METHOD_NOT_FOUND` | -1 | 方法不存在 |
| `PROPERTY_NOT_FOUND` | -2 | 属性不存在 |
| `INVALID_ARGUMENTS` | -3 | 参数无效 |
| `OBJECT_DESTROYED` | -4 | 对象已销毁 |
| `INTERNAL_ERROR` | -5 | 内部错误 |
| `USER_ERROR` | -100+ | 用户自定义错误 |

## 生命周期管理

```cpp
class ManagedObject : public CppObject {
public:
    ManagedObject() {
        bind_sync("ping", [](const json::Value&) {
            return json::Value("pong");
        });
    }

    std::string object_name() const override { return "managed"; }

    // 对象创建后自动调用
    void on_created() override {
        // 初始化资源
    }

    // 对象销毁前自动调用
    void on_destroyed() override {
        // 清理资源
    }
};
```

## 完整示例

见 `src/main.cpp`，包含:
- 数学服务 (同步/异步方法、属性)
- 文件服务 (异步读写)
- C++ 主动调用 JS
- 完整 HTML/CSS/JS 前端界面

## 依赖

| 依赖 | 版本 | 说明 |
|------|------|------|
| CMake | 3.20+ | 构建系统 |
| C++ 编译器 | C++17 | MSVC 2019+ / GCC 9+ / Clang 10+ |
| webview | latest | 单头文件库 |
| Windows SDK | 10+ | Win32 API |

## 许可证

MIT License
