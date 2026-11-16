@echo off
rem Renders test scenes through the driver on your graphics card and checks the
rem results. Does not need the driver to be installed. Add --window to also
rem open a window, or --bench to measure draw-call throughput.
cd /d "%~dp0"
selftest.exe vkon11.dll %*
echo.
pause
