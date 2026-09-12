@echo off
REM Read-only probe / flash backup helper. Same env dance as build.bat, but no
REM source mirror: esptool needs no project, only the toolchain env.
setlocal
set "MSYSTEM="
set "IDF_TOOLS_PATH=C:\Espressif"
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.5"
set "PYTHONUTF8=1"
set "PYTHONNOUSERSITE=True"
set "PYTHONPATH="
set "PYTHONHOME="
set "PATH=%IDF_TOOLS_PATH%\tools\idf-python\3.11.2;%PATH%"
call "%IDF_PATH%\export.bat" >nul
if errorlevel 1 (
  echo EXPORT_FAILED
  exit /b 1
)
python -m esptool %*
echo EXITCODE=%ERRORLEVEL%
