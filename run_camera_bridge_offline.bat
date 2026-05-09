@echo off
cd /d "%~dp0"

if "%~1"=="" (
    echo usage: run_camera_bridge_offline.bat ^<close_0.jpg^|close_1.jpg^|open_0.jpg^|open_1.jpg^>
    exit /b 1
)

python python_camera_bridge_offline.py %*
