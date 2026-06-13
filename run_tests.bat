@echo off
REM WebViewCpp C++ Service Test Runner
REM Runs built-in C++ tests via --test flag

set EXE=%~dp0build\Debug\WebViewCpp.exe

if not exist "%EXE%" (
    echo ERROR: WebViewCpp.exe not found at %EXE%
    echo Run build first: cmake --build build --config Debug
    exit /b 1
)

echo Running C++ Service Tests...
echo.

powershell -Command "
$p = Start-Process '%EXE%' -ArgumentList '--test' -PassThru -RedirectStandardOutput 'test_output.txt' -NoNewWindow;
$p.WaitForExit(10000) | Out-Null;
if ($p.HasExited) { exit $p.ExitCode } else { $p.Kill(); exit 1 }
"

if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Test process failed or timed out
    if exist test_output.txt type test_output.txt
    exit /b 1
)

echo.
echo Test output:
type test_output.txt

REM Check for failures
findstr "FAIL" test_output.txt >nul
if %ERRORLEVEL% EQU 0 (
    echo.
    echo WARNING: Some tests FAILED
    exit /b 1
)

echo.
echo All tests PASSED
exit /b 0
