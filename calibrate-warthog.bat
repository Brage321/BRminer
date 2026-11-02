@echo off
cd /d "%~dp0"
rem Quick provisional plan. Add --full to validate and save a profile.
rem Example: calibrate-warthog.bat --full --threads 14
brminer.exe --calibrate %*
pause
