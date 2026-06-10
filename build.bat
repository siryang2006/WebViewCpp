@echo off
REM ============================================================
REM  WebView C++ Binding - Build Script
REM  需要: CMake 3.20+, Visual Studio 2019/2022, Git
REM  依赖: nlohmann.json + WIL (自动 NuGet 下载)
REM         webview (自动 FetchContent 下载)
REM ============================================================

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

echo === WebView C++ Binding Build [%CONFIG%] ===
echo.

REM 创建构建目录
if not exist "build" mkdir build

REM CMake 配置
echo [1/3] Configuring with CMake...
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo CMake configure failed!
    cd ..
    pause
    exit /b 1
)

REM 编译
echo.
echo [2/3] Building %CONFIG%...
cmake --build . --config %CONFIG%
if errorlevel 1 (
    echo Build failed!
    cd ..
    pause
    exit /b 1
)

REM 完成
cd ..
echo.
echo [3/3] Done!
echo.
echo Output: build\%CONFIG%\WebViewCpp.exe
echo.
