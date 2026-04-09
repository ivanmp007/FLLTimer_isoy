@echo off
setlocal enabledelayedexpansion

set "DIR=%~dp0resources\musica_intro"
set "OUT=%DIR%\songs.js"

echo // Auto-generado por update_songs.bat — no editar manualmente> "%OUT%"
echo window.PRESENTATION_SONGS_LIST = [>> "%OUT%"

set FIRST=1
for %%F in ("%DIR%\*.mp3") do (
    if !FIRST!==0 echo ,>> "%OUT%"
    set FIRST=0
    <nul set /p ="  "%%~nxF"">> "%OUT%"
)

echo.>> "%OUT%"
echo ];>> "%OUT%"

echo Actualizado: %OUT%
pause
