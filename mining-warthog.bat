@echo off
setlocal

rem MINING TO BOBFARM.ICU
rem ===== EDIT THESE SETTINGS =====
set "PAYOUT_ADDRESS=YOUR-MINING-ADRESS"
set "THREADS=14"
set "NODE=mine.bobfarm.icu:3033"
rem Change NODE to another compatible Warthog Stratum host:port.
rem ===================================

echo.
echo  Soft Production 2026. All Rights Reserved
echo.

set "BRMINER=%~dp0brminer.exe"
if exist "%BRMINER%" (
    "%BRMINER%" --embedded-core-check 2>nul | findstr /X /C:"BRMINER_EMBEDDED_CORE=7" >nul
    if not errorlevel 1 goto miner_found
)

echo Could not find the current embedded-Core brminer.exe next to this launcher.
echo Download the matching BRminer release and place it next to this file.
pause
exit /b 1

:miner_found
if "%PAYOUT_ADDRESS%"=="ENTER-YOUR-WARTHOG-WALLET" (
    echo Set PAYOUT_ADDRESS to your Warthog receiving address in this file.
    pause
    exit /b 1
)

set "BRMINER_PAYOUT_ADDRESS=%PAYOUT_ADDRESS%"
set "BRMINER_THREADS=%THREADS%"
set "BRMINER_WART_NODE=%NODE%"
set "BRMINER_COIN=WART"

echo Starting Warthog mining on stratum+tcp://%NODE% for %PAYOUT_ADDRESS%.
"%BRMINER%"
set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo BRminer exited with code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
