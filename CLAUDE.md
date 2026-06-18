# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A C++20/JavaScript object-binding framework built on [webview/webview](https://github.com/webview/webview) (WebView2 on Windows). It lets you expose C++ objects to the page's JS context with compile-time type checking, bidirectional calls, and GC-synced lifetimes. `src/main.cpp` is both the demo app and the C++ unit-test suite; the GGUF model-download UI (`DownloadService`, `demo.html/css/js`, `models.json`) is a feature built on top of the framework.

Windows-only in practice: the build links a prebuilt `third_party/curl` + `zlib`, uses WebView2, and the CDP debug port is WebView2-specific. `main.cpp` keeps a `#else` POSIX `main()` but it isn't the supported path.

## Build & test

```bash
build.bat                 # Debug (default) — configures + builds via CMake + VS 2022 x64
build.bat Release
# raw equivalent: cmake -S . -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Debug
```

First configure auto-downloads `nuget.exe`, then NuGet (`nlohmann.json`, Microsoft WIL) and FetchContent (`webview` 0.12.0). Output: `build\<Config>\WebViewCpp.exe`. A POST_BUILD step copies `demo.html/css/js`, `DownloadService.js`, and `models.json` next to the exe — these are loaded at runtime from the exe directory via `file://`, so **rebuild (or re-copy) after editing any frontend file**.

```bash
run_tests.bat             # C++ unit tests — runs WebViewCpp.exe --test (no WebView needed)
WebViewCpp.exe --test     # same, direct

test_cdp.bat [port]       # CDP end-to-end: drives real WebView2 over Chrome DevTools Protocol (default 9222)
python tests/test_cdp.py --port=9222          # needs: pip install websockets
python tests/test_download_e2e.py             # download start/pause/resume/cancel over CDP (port 9223)
```

The C++ tests live inline in `run_cpp_tests()` in `src/main.cpp` (no test framework) — add cases there. To debug JS↔C++ live, launch with `--cdp-port=<port>` and attach a CDP client / `chrome://inspect`.

## Architecture

Three layers, bottom-up:

1. **`CppObject` (`include/binding/CppObject.h`)** — base class for anything exposed to JS. All binding logic is header-only template metaprogramming:
   - `bind_sync(name, lambda)` — return value sent back synchronously. **The lambda's signature drives behavior**: a single `const json&` arg = type-erased mode (you parse args yourself); typed args (`int`, `double`, `bool`, `std::string`, `json`) = type-safe mode with compile-time checks (`static_assert` rejects pointers/refs/void args) and detailed runtime errors (`[name] Argument N type mismatch: expected X, got Y`).
   - `bind_async(name, lambda)` — lambda is `(const std::string& id, const json& args, WebViewWrapper* wv)`; you call `wv->resolve(id, ...)` / `wv->reject(id, ...)` later, usually from inside `wv->dispatch_task(...)`. **`args` is always a JSON array** of the JS call arguments.
   - `bind_property(name, getter[, setter])` — exposed as a JS property (but access is async/Promise-based, see below).
   - Errors are thrown as `BindingException` with an `ErrorCode`; `ok_result()` / `error_result()` produce the standard `{ok, code, message}`/`{ok, data}` envelopes. `DownloadService` returns these envelopes directly; the demo services in `main.cpp` mostly use raw `resolve`/`reject`.

2. **`WebViewWrapper` (`include/WebViewWrapper.h`, `src/WebViewWrapper.cpp`)** — owns the webview, injects the JS bridge, routes calls.
   - `bind_object(shared_ptr)` registers a singleton under `obj->object_name()`; `bind_factory("Worker", create, Instance)` lets JS do `new window.__cpp__.Worker(...)` with each instance GC'd via `FinalizationRegistry` → C++ `~Worker()`.
   - **Threading model is the critical invariant**: webview/WebView2 API calls must happen on the GUI thread. The wrapper records the GUI thread id and routes injection through `webview_dispatch`. Background work goes through `dispatch_task()` (a dedicated worker thread + queue) rather than `std::thread::detach`, so tasks are cancelled and resolve/reject is suppressed once the webview is torn down. Always guard late callbacks with `wv->is_ready()`. `bind_object`/`bind_factory` are safe to call from any thread.
   - C++→JS: `call_registered_js(name, args)` invokes a JS fn the page registered via `window.__register_cb__(name, fn)` (stored in `window.__registered_cbs__`); `call_js`/`call_js_fn` invoke arbitrary JS with result/error callbacks; `eval` runs raw JS.

3. **JS bridge (injected, see `setup_js_bridge` / `inject_single_object` in `WebViewWrapper.cpp`)** — builds `window.__cpp__.<object>.<method>()`. **Every method and property access returns a Promise** (webview_bind is async), so JS must `await` even property reads — `JSON.stringify` of a bound object will not capture property values. Persistent injection (`webview_init`) re-runs on every document load so bindings survive navigation; `post_eval` also applies them to the already-loaded page.

### Download feature specifics

- `DownloadService` (C++) runs one `std::thread` per `modelId`, writes via libcurl with `CURLOPT_RESUME_FROM_LARGE` for resume, and reports progress by calling back into a JS callback id passed in the `startDownload` args. Pause/resume is implemented by blocking the curl write/progress callbacks on a `condition_variable`; cancel sets an atomic flag the callbacks check.
- `DownloadService.js` wraps the raw bridge into a friendlier `window.downloadService` API and manages callback registration/cleanup.
- `ConfigService` in `main.cpp` owns `models.json` (read with on-the-fly `status` detection by checking whether the GGUF file exists, plus `addModel`/`deleteModel`/`deleteFile`). Model files download under `downloads/<modelId>/`.

## Conventions

- Existing code comments and docs are largely in Chinese; match the surrounding language when editing a file.
- `third_party/` (curl, zlib, webview.h, WebView2 SDK) is vendored — don't treat it as project code to review or modify.
- Keep `DownloadService.js` and the C++ `DownloadService` method signatures in lockstep — they're a hand-maintained 1:1 mapping.
