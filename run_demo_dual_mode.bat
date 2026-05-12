@echo off
setlocal
cd /d "%~dp0"

call .\run_live.bat --dual --demo-io-loop --auto-exit-ms 4000 --log-file demo_dual_mode_log.txt

