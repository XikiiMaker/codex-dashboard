@echo off
REM ESP-IDF build entry for panel-codex.
REM
REM The repo lives under a path containing a space, and ESP-IDF on Windows does
REM not survive that any better than it survives Chinese characters. Everything
REM below exists to work around it:
REM
REM 1) ASCII_ROOT. The build cannot happen under the repo path at all, so this
REM    script mirrors the sources to a clean directory, builds there, and copies
REM    the artifacts back. CMake wraps the over-long sections.ld command in a
REM    generated .bat, and cmd.exe mis-splits the unquoted path there, spinning
REM    at 100% CPU on one core forever without ever exiting.
REM    A junction or a subst drive will NOT do: idf.py realpaths the project dir
REM    before handing it to CMake, so both were measured to resolve straight back
REM    to the original path. Only a real copy works. Judge the mirror by CMake
REM    printing "Build files have been written to: C:/espcodex".
REM    The directory deliberately differs from panel-firmware's C:\espfw so the
REM    two projects never clobber each other's build tree.
REM 2) MSYSTEM must be empty and this must run under cmd.exe. idf_tools.py
REM    aborts on sight of MSYSTEM, and Git Bash cannot clear it for children.
REM 3) PYTHONUTF8. CMake writes build/config.env as UTF-8, but
REM    prepare_kconfig_files.py reads it via argparse.FileType, which uses the
REM    cp936 locale codec. UTF-8 mode makes that read round-trip.
REM 4) IDF_CCACHE_ENABLE=0. ccache uses std::filesystem and aborts with
REM    "Illegal byte sequence" on paths it cannot decode; PYTHONUTF8 cannot help
REM    a C++ binary. Kept as a belt-and-braces guard.
REM 5) --no-hints. idf.py otherwise drains ninja's stdio through an asyncio
REM    reader to mine "hints", which buries the real error behind a pipe.
setlocal
set "ASCII_ROOT=C:\espcodex"
for %%I in ("%~dp0.") do set "PANEL_SRC=%%~fI"

echo === mirroring sources to %ASCII_ROOT% ===
robocopy "%PANEL_SRC%" "%ASCII_ROOT%" /MIR /NFL /NDL /NJH /NJS /NP /XD build .git /XF *.log
if errorlevel 8 (
  echo MIRROR_FAILED
  exit /b 1
)
if not exist "%ASCII_ROOT%\CMakeLists.txt" (
  echo MIRROR_FAILED
  exit /b 1
)

set "MSYSTEM="
set "IDF_TOOLS_PATH=C:\Espressif"
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.5"
set "PYTHONUTF8=1"
set "IDF_CCACHE_ENABLE=0"
set "PYTHONNOUSERSITE=True"
set "PYTHONPATH="
set "PYTHONHOME="
set "PATH=%IDF_TOOLS_PATH%\tools\idf-python\3.11.2;%IDF_TOOLS_PATH%\tools\idf-git\2.44.0\cmd;%PATH%"

call "%IDF_PATH%\export.bat"
if errorlevel 1 (
  echo EXPORT_FAILED
  exit /b 1
)
cd /d "%ASCII_ROOT%"
call idf.py --no-hints %*
set "IDF_RC=%ERRORLEVEL%"

REM Bring the flashable images back so the repo, not C:\espcodex, is what gets
REM archived and flashed. Missing files are not an error: reconfigure and
REM menuconfig legitimately produce none of them.
if exist "%ASCII_ROOT%\build\codex_panel.bin" (
  echo === copying artifacts back ===
  if not exist "%PANEL_SRC%\build\bootloader\" mkdir "%PANEL_SRC%\build\bootloader"
  if not exist "%PANEL_SRC%\build\partition_table\" mkdir "%PANEL_SRC%\build\partition_table"
  for %%F in (codex_panel.bin codex_panel.elf codex_panel.map flasher_args.json ota_data_initial.bin) do (
    if exist "%ASCII_ROOT%\build\%%F" copy /y "%ASCII_ROOT%\build\%%F" "%PANEL_SRC%\build\%%F" >nul
  )
  if exist "%ASCII_ROOT%\build\bootloader\bootloader.bin" copy /y "%ASCII_ROOT%\build\bootloader\bootloader.bin" "%PANEL_SRC%\build\bootloader\bootloader.bin" >nul
  if exist "%ASCII_ROOT%\build\partition_table\partition-table.bin" copy /y "%ASCII_ROOT%\build\partition_table\partition-table.bin" "%PANEL_SRC%\build\partition_table\partition-table.bin" >nul
)

echo EXITCODE=%IDF_RC%
exit /b %IDF_RC%
