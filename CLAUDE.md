# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A C++20/JavaScript object-binding framework built on [webview/webview](https://github.com/webview/webview) (WebView2 on Windows). It lets you expose C++ objects to the page's JS context with compile-time type checking, bidirectional calls, and GC-synced lifetimes. `src/main.cpp` is both the demo app and the C++ unit-test suite; the GGUF model-download UI and local LLM inference features are built on top of the framework.

Windows-only in practice: the build links a prebuilt `third_party/curl` + `zlib`, uses WebView2, and the CDP debug port is WebView2-specific.

## Build & test

```bash
build.bat                 # Debug (default) — configures + builds via CMake + VS 2022 x64
build.bat Release
```

Output: `build\<Config>\WebViewCpp.exe`. A POST_BUILD step copies frontend files next to the exe — **rebuild after editing any frontend file**.

```bash
python tests/run_tests.py  # C++ unit tests (no WebView needed)
WebViewCpp.exe --test     # same, direct

test_cdp.bat [port]       # CDP end-to-end tests (default 9222)
python tests/test_cdp.py   # CDP integration tests (requires built exe)
```

## Architecture

Three layers, bottom-up:

1. **`CppObject` (`include/binding/CppObject.h`)** — base class for anything exposed to JS. All binding logic is header-only template metaprogramming:
   - `bind_sync(name, lambda)` — return value sent back synchronously. A single `const json&` arg = type-erased mode; typed args = type-safe mode with `static_assert` checks.
   - `bind_async(name, lambda)` — lambda is `(id, args, wv)`; call `wv->resolve(id, ...)` / `wv->reject(id, ...)` later.
   - `bind_property(name, getter[, setter])` — exposed as JS property (async/Promise-based).
   - Errors: `BindingException` with `ErrorCode`; `ok_result()` / `error_result()` produce `{ok, data}` / `{ok, code, message}`.

2. **`WebViewWrapper` (`include/WebViewWrapper.h`, `src/WebViewWrapper.cpp`)** — owns the webview, injects JS bridge.
   - `bind_object(shared_ptr)` / `bind_factory("Type", create, Instance)` — register C++ objects to JS.
   - **Threading invariant**: webview API calls must be on GUI thread. Background work goes through `dispatch_task()` (worker thread + queue).
   - C++→JS: `call_registered_js(name, args)` / `call_js()` / `call_js_fn()`.
   - **Window size**: creates own HWND before `webview_create(owns_window=false)` to prevent startup flicker.
   - **Job Object**: `KILL_ON_JOB_CLOSE` ensures all child processes (llama-server) are killed when parent exits.

3. **JS bridge (injected in `setup_js_bridge` / `inject_single_object`)** — builds `window.__cpp__.<object>.<method>()`. **Every call returns a Promise** — JS must `await`. Persistent injection survives navigation.

### Services

- **`DownloadService`** — libcurl download with pause/resume/cancel. One thread per `modelId`, progress via JS callback.
- **`ChatService`** — llama-server/llama-box subprocess management + HTTP streaming inference. **Dual backend**: `llama-server.exe` for LLM/chat models, `llama-box.exe` for diffusion/image models (FLUX). Backend selected via `backend` param in `startModel()`. `generateImage({modelId?, prompt, callback})` calls `/v1/images/generations` on a running FLUX model. **Multi-model**: runs several instances concurrently, keyed by `modelId`, each with its own port + `Subprocess` + metrics. `startModel({modelId, backend?,...})` / `stopModel({modelId?})` (no modelId = stop all) / `chat({modelId?, prompt, callback})`. `getStatus`/`getMetrics` take an optional `modelId`: with it they return one model's object; without it they return `{status, models:[...]}` (one entry per running model). Per-model metrics include `memoryMB`, `cpuPercent`, `gpuMemoryMB`, `threads`, `handles`, `port`, and `pid`.
- **`Subprocess`** — reusable child process manager (start/stop/monitor, process metrics).
- **`ConfigService`** — manages `models.json`, detects file existence for model status.

### Subprocess metrics

`Subprocess::getMetrics()` collects:
- Memory (MB) via `GetProcessMemoryInfo`
- CPU % via `GetProcessTimes` (sampled delta)
- GPU memory (MB) via `nvidia-smi` (NVIDIA only)
- Thread/handle counts via `CreateToolhelp32Snapshot`

## Project structure

```
src/
  main.cpp              # Demo app + C++ unit tests
  WebViewWrapper.cpp    # Framework
  DownloadService.cpp/h  # Model download
  ChatService.cpp/h     # LLM inference
  Subprocess.cpp/h      # Child process manager
  demo.html/css/js      # Frontend UI
  DownloadService.js     # Download JS wrapper
  ChatService.js         # Chat JS wrapper
  js/
    core.js             # Shared utilities, AppState, AppBus event system
    ui.js               # Theme, cards, tabs, mode selector
    chat.js             # Chat UI + streaming rendering
    models.js            # Model list, download, detail page
    config.js           # Launch config modal
    service-panel.js     # Sidebar service status + metrics
  services/
    MathService.h       # Math operations demo
    FileService.h        # File operations demo
    ConfigService.h      # Config read/write
    WorkerService.h     # Factory pattern demo
include/
  WebViewWrapper.h
  binding/CppObject.h
tests/
  run_tests.py          # C++ unit test runner
  test_cdp.py            # CDP integration tests
  test_download_e2e.py    # Download E2E tests
```

## Known fixes

| Fix | Description |
|-----|-------------|
| ChatService working dir | `startServer` passes exe dir as subprocess cwd; relative gguf paths resolved against it (else model file + llama-bin DLLs not found) |
| nlohmann object vs array | `ok_result({{ {"a",1},{"b",arr} }})` builds an **array** when a value is itself array/object — build results via explicit `json::object()` + `d["k"]=v` to force an object |
| Pending callback registration | `__webview_async_call__`: register callback BEFORE object lookup |
| Callback lock order | `__cpp_result__`: invoke callbacks OUTSIDE `m_callback_mutex` |
| JSON.stringify(undefined) | Coalesce to `null` in `call_js` / `call_js_fn` |
| JSON object init | `{"key", val}` not `{{"key", val}}` in nlohmann json — latter creates arrays |
| Download destructor deadlock | Copy task ptrs out, release lock, then join |
| Resume corruption | Lazy file open after headerCallback confirms HTTP 206 |
| Path traversal in ConfigService | Use GetFullPathNameA to validate final path stays within base_dir |
| nvidia-smi detection race | Protect static variables with mutex for thread-safe one-time init |
| curl error messages | Use `curl_easy_strerror()` for human-readable error output |
| llama-box FLUX warmup timeout | Pass `--no-warmup` to llama-box: CPU warmup of diffusion models (one full empty run) is extremely slow and trips the health-check timeout before the server is actually ready |

## Conventions

- Comments are in Chinese — match when editing.
- `third_party/` (curl, zlib, webview, WebView2 SDK) is vendored — don't modify.
- Frontend files copied to `build/Debug/` by CMake POST_BUILD.
- llama-server binary goes next to the exe. Download from https://github.com/ggml-org/llama.cpp/releases
- llama-box binary goes next to the exe for FLUX/image models. Download from https://github.com/gpustack/llama-box/releases
- Model files go in `downloads/<modelId>/` subdirectory of exe location.
