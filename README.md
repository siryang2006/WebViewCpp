# WebViewCpp

基于 [webview/webview](https://github.com/webview/webview) 的 C++ / JavaScript 对象绑定框架，支持 **C++20 编译期类型检查**、**双向弱引用生命周期同步**、**工厂模式动态创建实例**。

## 特性

- **同步/异步方法绑定** — `bind_sync()` / `bind_async()`
- **C++20 编译期类型检查** — lambda 签名自动推导，参数类型/数量不匹配编译报错
- **运行时详细错误信息** — 类型不匹配时显示 expected/got 具体类型
- **属性绑定** — 只读/读写属性，JS 端用 `.` 直接访问
- **工厂模式** — JS `new` 创建 C++ 实例，GC 自动清理
- **生命周期同步** — 双向弱引用，任一方销毁另一方自动失效
- **C++ ↔ JS 双向调用** — C++ 主动调用 JS 回调，支持 Promise
- **线程安全** — 内置任务派发队列，替代 `std::thread::detach`

## 架构

```
┌─────────────────────────────────────────────────────┐
│                    Your Application                  │
│  ┌───────────┐  ┌───────────┐  ┌─────────────────┐ │
│  │MathService│  │FileService│  │new Worker("Alice")│ │
│  └─────┬─────┘  └─────┬─────┘  └────────┬────────┘ │
│        │              │                  │           │
│  ┌─────┴──────────────┴──────────────────┴────────┐ │
│  │              CppObject 基类                     │ │
│  │  bind_sync("add", [](int a, int b){...})       │ │
│  │  bind_async("fetch", [](id, args, wv){...})    │ │
│  │  bind_property("version", []()->string{...})   │ │
│  │  bind_factory("Worker", Worker::create)        │ │
│  └───────────────────┬────────────────────────────┘ │
│                      │                               │
│  ┌───────────────────┴────────────────────────────┐ │
│  │           WebViewWrapper (核心封装)             │ │
│  │  bind_object()  resolve()  reject()            │ │
│  │  call_js()      eval()     dispatch_task()     │ │
│  └───────────────────┬────────────────────────────┘ │
│                      │                               │
│  ┌───────────────────┴────────────────────────────┐ │
│  │           JS Bridge (自动注入)                  │ │
│  │  window.__cpp__.math.add(10, 20)               │ │
│  │  new window.__cpp__.Worker("Alice", 5)         │ │
│  │  FinalizationRegistry → 自动 GC 清理            │ │
│  └───────────────────┬────────────────────────────┘ │
└──────────────────────┼───────────────────────────────┘
                       │
┌──────────────────────┴───────────────────────────────┐
│  webview (底层引擎，via FetchContent)                  │
│  Windows: WebView2  |  macOS: WebKit  |  Linux: GTK   │
└──────────────────────────────────────────────────────┘
```

## 快速开始

### 构建

```bash
build.bat          # Debug
build.bat Release  # Release
```

首次运行自动下载：
- [nlohmann/json](https://github.com/nlohmann/json) (NuGet)
- [Microsoft WIL](https://github.com/microsoft/wil) (NuGet)
- [webview/webview](https://github.com/webview/webview) (FetchContent)

### 定义 C++ 对象

```cpp
#include "WebViewWrapper.h"

class MathService : public CppObject {
public:
    MathService() {
        // 同步方法：C++20 类型检查
        bind_sync("add", [](int a, int b) { return a + b; });
        bind_sync("multiply", [](double a, double b) { return a * b; });

        // 异步方法：后台线程 + resolve/reject
        bind_async("slow_add", [](const std::string& id,
                                   const json& args, WebViewWrapper* wv) {
            int a = args[0].get<int>();
            int b = args[1].get<int>();
            wv->dispatch_task([id, a, b, wv]() {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                wv->resolve(id, a + b);   // 成功
                // wv->reject(id, "error"); // 失败
            });
        });

        // 属性
        bind_property("version", []() -> std::string { return "1.0"; });
        bind_property("pi", []() -> double { return 3.14159; });
    }

    std::string object_name() const override { return "math"; }

    void on_created() override { std::cout << "MathService created\n"; }
    void on_destroyed() override { std::cout << "MathService destroyed\n"; }
};
```

### 注册并运行

```cpp
int main() {
    WebViewWrapper wv;
    wv.init("My App", "", 800, 600);

    // 全局单例
    wv.bind_object(std::make_shared<MathService>());

    wv.run();
    return 0;
}
```

### JS 端调用

```javascript
// 同步调用（类型安全）
const sum = window.__cpp__.math.add(10, 20);  // 30
const product = window.__cpp__.math.multiply(3.14, 2);  // 6.28

// 异步调用（Promise）
const result = await window.__cpp__.math.slow_add(1, 2);

// 读取属性
console.log(window.__cpp__.math.version);  // "1.0"
console.log(window.__cpp__.math.pi);       // 3.14159
```

## 类型安全绑定

### 支持类型

| C++ 类型 | JS 类型 | 说明 |
|----------|---------|------|
| `int` | number (integer) | 整数 |
| `double` | number (float) | 浮点数 |
| `bool` | boolean | 布尔值 |
| `std::string` | string | 字符串 |
| `json` | any | 任意 JSON 值 |
| `json` (返回值) | any | 任意 JSON 值 |
| `void` (返回值) | undefined | 无返回值 |

### 两种模式

```cpp
// 1. 类型安全模式（推荐）— 编译期检查 + 详细运行时错误
bind_sync("add", [](int a, int b) { return a + b; });

// 2. 类型擦除模式（灵活）— 手动解析 JSON
bind_sync("raw", [](const json& args) {
    return args[0].get<int>() + args[1].get<int>();
});
```

检测逻辑：lambda 第一个参数是 `const json&` → 类型擦除；否则 → 类型安全。

### 编译期检查

```cpp
bind_sync("add", [](int a, int b) { return a + b; });
```

| 检查项 | 失败示例 | 错误信息 |
|--------|---------|---------|
| 参数数量 | `add(1)` | `[add] Expected 2, got 1` |
| 参数类型 | `add("x", 2)` | `[add] Arg 0: expected int, got string` |
| 禁止指针 | `[](int* p)` | `static_assert: pointer types not allowed` |

## 工厂模式（JS new 创建 C++ 实例）

```cpp
class WorkerService : public CppObject {
public:
    WorkerService(const std::string& name, int priority)
        : m_name(name), m_priority(priority) {
        bind_sync("getName", [this](const json&) { return m_name; });
        bind_sync("getPriority", [this](const json&) { return m_priority; });
        bind_async("doWork", [this](const std::string& id,
                                     const json& args, WebViewWrapper* wv) {
            wv->dispatch_task([id, this, wv]() {
                wv->resolve(id, {{"worker", m_name}, {"status", "done"}});
            });
        });
    }

    static std::shared_ptr<WorkerService> create(const json& args) {
        std::string name = args[0].get<std::string>();
        int priority = args[1].get<int>();
        return std::make_shared<WorkerService>(name, priority);
    }

    std::string object_name() const override { return "worker_" + m_name; }
    void on_destroyed() override { std::cout << "Worker " << m_name << " GC'd\n"; }

private:
    std::string m_name;
    int m_priority;
};

// 注册工厂
wv.bind_factory("Worker", WorkerService::create);
```

JS 端：
```javascript
const worker = new window.__cpp__.Worker("Alice", 5);
console.log(worker.getName());   // "Alice"
await worker.doWork("build");    // {worker: "Alice", status: "done"}

// JS GC 后自动清理 C++ 对象（FinalizationRegistry）
worker = null;  // C++ ~WorkerService() 自动调用
```

## C++ 调用 JS

```cpp
// 方式 1: 调用已注册的 JS 回调
// JS: window.__register_cb__("onUpdate", (data) => { ... });
wv.call_registered_js("onUpdate", {{"event", "click"}, {"x", 100}});

// 方式 2: 调用任意 JS 函数（带回调）
wv.call_js("myFunc", {"hello", 42},
    [](const json& result) { /* 成功 */ },
    [](const std::string& err) { /* 失败 */ }
);

// 方式 3: 直接执行 JS
wv.eval("document.title = 'Updated'");
```

## 线程安全

```cpp
// 使用 dispatch_task 替代 std::thread::detach
bind_async("heavy_work", [](const std::string& id,
                             const json& args, WebViewWrapper* wv) {
    wv->dispatch_task([id, wv]() {
        // 后台线程执行
        auto result = do_heavy_computation();
        wv->resolve(id, result);
    });
});
```

`dispatch_task` 保证：
- WebView 销毁时未执行的任务自动取消
- 不会在 WebView 已销毁后调用 resolve/reject

## 项目结构

```
WebViewCpp/
├── CMakeLists.txt              # 构建配置 (CMake 3.20+)
├── build.bat                   # Windows 构建脚本
├── include/
│   ├── WebViewWrapper.h        # 核心封装类
│   └── binding/
│       └── CppObject.h         # 基类 (类型安全绑定 + 工厂)
├── src/
│   ├── WebViewWrapper.cpp      # 实现 (JS Bridge + 生命周期)
│   └── main.cpp                # 示例程序
└── docs/
    └── API.md                  # API 参考文档
```

## 依赖

| 依赖 | 获取方式 | 说明 |
|------|---------|------|
| CMake 3.20+ | 系统安装 | 构建系统 |
| C++20 编译器 | MSVC 2022+ | 编译期类型检查需要 C++20 |
| webview/webview | FetchContent (自动) | WebView2/WebKit 封装 |
| nlohmann/json | NuGet (自动) | JSON 序列化 |
| Microsoft WIL | NuGet (自动) | Windows 工具库 |

## 许可证

MIT License
