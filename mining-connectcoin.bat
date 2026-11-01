@echo off
setlocal

rem MINING TO MESHPOOL.NET
rem ===== EDIT THESE TWO SETTINGS =====
set "PAYOUT_ADDRESS=YOUR-WALLET-ADRESS"
set "THREADS=14"
rem ===================================

echo.
echo  ©️ Soft Production 2026. All Rights Reserved
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
rem DO NOT EDIT BELOW THIS LINE UNLESS YOU KNOW WHAT YOU ARE DOING!!

if "%PAYOUT_ADDRESS%"=="ENTER-YOUR-CONNECTCOIN-WALLET" (
    echo Set PAYOUT_ADDRESS to your ConnectCoin receiving address in this file.
    pause
    exit /b 1
)

set "BRMINER_PAYOUT_ADDRESS=%PAYOUT_ADDRESS%"
set "BRMINER_THREADS=%THREADS%"
set "BRMINER_COIN=CONNECTCOIN_POOL"

echo Starting ConnectCoin pool mining on stratum+tcp://pool.meshpool.net:3357.
"%BRMINER%"
set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo BRminer exited with code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
