@echo off
cd /d "%~dp0"
rem Offline benchmark: 30 seconds warmup, then five measured minutes.
rem Optional example: benchmark-warthog.bat --threads 14
brminer.exe --benchmark --backend cuda --duration 300 --warmup 30 --json benchmark.json %*
pause
