@echo off
cd /d "%~dp0"
rem Replace the wallet below with your Warthog payout address.
brminer.exe -a warthog -p mine.bobfarm.icu:3033 -w ENTER-YOUR-WARTHOG-WALLET
pause
