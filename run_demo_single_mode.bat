@echo off
setlocal
cd /d "%~dp0"

call .\run_live.bat --single --demo-io-loop --auto-exit-ms 4000 --log-file demo_single_mode_log.txt

