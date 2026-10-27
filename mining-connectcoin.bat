@echo off
setlocal

echo.
echo  ©️ Soft Production 2026. All Rights Reserved
echo.

set "BRMINER=%~dp0brminer.exe"
if exist "%BRMINER%" (
    "%BRMINER%" --embedded-core-check 2>nul | findstr /X /C:"BRMINER_EMBEDDED_CORE=7" >nul
    if not errorlevel 1 goto miner_found
)

set "BRMINER=%~dp0build-msvc\Release\brminer.exe"
if exist "%BRMINER%" (
    "%BRMINER%" --embedded-core-check 2>nul | findstr /X /C:"BRMINER_EMBEDDED_CORE=7" >nul
    if not errorlevel 1 goto miner_found
)

echo Could not find the current embedded-Core brminer.exe.
echo Build it with: cmake --build build-msvc --config Release --target brminer
pause
exit /b 1

:miner_found
rem Solo mining settings. Edit the payout address and thread count with Notepad.
set "PAYOUT_ADDRESS=cc1pkh240vqmu5zsvmtlk7f38704v46wa2cnuvvnnglq66nzzaa7zxcqsr59a6"
set "THREADS=14"

rem DO NOT EDIT BELOW THIS LINE UNLESS YOU KNOW WHAT YOU ARE DOING!!

if "%PAYOUT_ADDRESS%"=="" (
    echo Set PAYOUT_ADDRESS to your ConnectCoin receiving address in this file.
    pause
    exit /b 1
)

set "BRMINER_PAYOUT_ADDRESS=%PAYOUT_ADDRESS%"
set "BRMINER_THREADS=%THREADS%"
set "BRMINER_DEFAULT_CHAIN=-chain=main"
for %%A in (%*) do (
    if /I "%%~A"=="-chain=main" set "BRMINER_DEFAULT_CHAIN="
    if /I "%%~A"=="-chain=test" set "BRMINER_DEFAULT_CHAIN="
    if /I "%%~A"=="-chain=testnet4" set "BRMINER_DEFAULT_CHAIN="
    if /I "%%~A"=="-chain=signet" set "BRMINER_DEFAULT_CHAIN="
    if /I "%%~A"=="-chain=regtest" set "BRMINER_DEFAULT_CHAIN="
    if /I "%%~A"=="-testnet" set "BRMINER_DEFAULT_CHAIN="
    if /I "%%~A"=="-testnet4" set "BRMINER_DEFAULT_CHAIN="
    if /I "%%~A"=="-signet" set "BRMINER_DEFAULT_CHAIN="
    if /I "%%~A"=="-regtest" set "BRMINER_DEFAULT_CHAIN="
)

echo Starting the embedded ConnectCoin Core node and CPU miner.
"%BRMINER%" %BRMINER_DEFAULT_CHAIN% %*
set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo BRminer exited with code %EXIT_CODE%.
pause
exit /b %EXIT_CODE%
