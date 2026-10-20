@echo off
rem Launch Kconfig menuconfig for a profile.
rem   tools\menuconfig.cmd            -> profile f407ve
rem   tools\menuconfig.cmd host       -> profile host
setlocal
cd /d "%~dp0.."

set "PROFILE=%~1"
if "%PROFILE%"=="" set "PROFILE=f407ve"

python tools\fwconfig.py menuconfig --profile "%PROFILE%"
if errorlevel 1 (
    echo.
    echo menuconfig failed for profile "%PROFILE%".
    echo Install curses first: python -m pip install -r tools\requirements.txt
    pause
)
endlocal
