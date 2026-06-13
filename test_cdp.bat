@echo off
REM ============================================================
REM  WebView C++ Binding - CDP Test Runner
REM  需要: Python 3.7+, pip install websockets
REM ============================================================

set PORT=%1
if "%PORT%"=="" set PORT=9222

echo === WebView C++ Binding CDP Tests [port=%PORT%] ===
echo.

REM 检查 Python
python --version >nul 2>&1
if errorlevel 1 (
    echo Python not found! Please install Python 3.7+.
    pause
    exit /b 1
)

REM 检查 websockets 库
python -c "import websockets" >nul 2>&1
if errorlevel 1 (
    echo Installing websockets library...
    pip install websockets
)

echo Running tests on port %PORT%...
echo.

python tests/test_cdp.py --port=%PORT%
set RESULT=%errorlevel%

echo.
if %RESULT% equ 0 (
    echo All tests passed!
) else (
    echo Some tests failed!
)

pause
exit /b %RESULT%
