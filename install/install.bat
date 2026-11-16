@echo off
rem Registers VKOn11 with the Vulkan loader (64-bit applications).
rem Keep this folder where it is after installing: the registry points at it.
net session >nul 2>&1
if errorlevel 1 (
    echo This script needs administrator rights.
    echo Right-click install.bat and choose "Run as administrator".
    pause
    exit /b 1
)
if not exist "%~dp0vkon11.dll" (
    echo vkon11.dll was not found next to this script.
    pause
    exit /b 1
)
reg add "HKLM\SOFTWARE\Khronos\Vulkan\Drivers" /v "%~dp0vkon11_icd.json" /t REG_DWORD /d 0 /f /reg:64
if errorlevel 1 (
    echo Registration failed.
    pause
    exit /b 1
)
echo.
echo VKOn11 is registered. Vulkan applications will now see your graphics card
echo with "(VKOn11)" after its name.
if not exist "%SystemRoot%\System32\vulkan-1.dll" (
    echo.
    echo NOTE: the Vulkan loader ^(vulkan-1.dll^) is not installed on this PC.
    echo Install the "Vulkan Runtime" from https://vulkan.lunarg.com/sdk/home
    echo ^(Windows, "Runtime Installer"^), otherwise Vulkan programs cannot start.
)
pause
