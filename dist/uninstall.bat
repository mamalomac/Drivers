@echo off
rem Removes the VKOn11 registration. Nothing else on the system was changed.
net session >nul 2>&1
if errorlevel 1 (
    echo This script needs administrator rights.
    echo Right-click uninstall.bat and choose "Run as administrator".
    pause
    exit /b 1
)
reg delete "HKLM\SOFTWARE\Khronos\Vulkan\Drivers" /v "%~dp0vkon11_icd.json" /f /reg:64
echo VKOn11 is no longer registered. You can delete this folder.
pause
