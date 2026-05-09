@echo off
cd /d "%~dp0"

if "%~1"=="" (
    echo usage: run_microphone_bridge_offline.bat ^<huu.m4a^|ahh.m4a^|"no voice.m4a"^>
    exit /b 1
)

python python_microphone_bridge_offline.py %*
