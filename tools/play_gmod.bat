@echo off
rem Starts Garry's Mod for Garry's Redemption: 64-bit, windowed, on an empty map. Put this
rem file next to gmod_win64.exe (the release zip does) and run it after RDR2 story mode has
rem loaded. The size here is only what GMod starts at: once linked it takes RDR2's size and
rem its window is hidden until RDR2 closes.
set W=%1
set H=%2
if "%W%"=="" set W=1920
if "%H%"=="" set H=1080
rem No parenthesised block here: the usual Steam path has "(x86)" in it, which ends one early.
if exist "%~dp0gmod_win64.exe" goto run
echo gmod_win64.exe is not next to this file. Switch Garry's Mod to the x86-64 branch in Steam: Properties, Betas.
pause
exit /b 1
:run
start "" /d "%~dp0." "%~dp0gmod_win64.exe" -windowed -w %W% -h %H% -novid -condebug +maxplayers 1 +map gm_flatgrass
