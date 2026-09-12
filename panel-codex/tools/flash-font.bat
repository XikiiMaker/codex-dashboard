@echo off
REM Flash the CJK font into the "font" partition (offset 0x620000).
REM Keep this file pure ASCII: cmd.exe mangles non-ASCII bytes in batch files.
REM
REM The font is NOT part of the firmware image and does NOT travel with OTA.
REM Flash it once per board, then firmware updates can go over the air freely.
REM
REM Usage:  tools\flash-font.bat COM4
setlocal

set "PORT=%~1"
if "%PORT%"=="" (
    echo [ERROR] usage: tools\flash-font.bat ^<PORT^>   e.g. tools\flash-font.bat COM4
    exit /b 1
)

set "FONT=%~dp0NotoSansSC-Regular.otf"
if not exist "%FONT%" (
    echo [ERROR] font not found: %FONT%
    echo         The otf is not in git ^(8MB^). Download Noto Sans SC Regular
    echo         and drop it here, see panel-firmware\README.md
    exit /b 1
)

REM Offset must match the "font" row in partitions.csv.
set "OFFSET=0x620000"

echo Flashing %FONT%
echo   port   %PORT%
echo   offset %OFFSET%
echo.

call "%~dp0..\esptool.bat" --chip esp32p4 -p %PORT% -b 460800 write_flash %OFFSET% "%FONT%"
if errorlevel 1 (
    echo.
    echo [ERROR] flash failed
    exit /b 1
)

echo.
echo [OK] font flashed. Reboot the panel; the "CJK font not loaded" toast should be gone.
