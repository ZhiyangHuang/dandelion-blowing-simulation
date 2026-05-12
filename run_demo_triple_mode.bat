@echo off
setlocal
cd /d "%~dp0"

call .\run_live.bat --triple --demo-io-loop --auto-exit-ms 4000 --log-file demo_triple_mode_log.txt

