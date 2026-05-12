@echo off
REM ==================================================================
REM  Wii proto-disc pipeline -- rvthtool + wit + custom disc ID rename
REM  Part of: usbloadergx-dev-disc-support
REM  License: GPL-3.0-or-later
REM
REM  Usage:
REM    1. Drag a .iso / .gcm / .rvm onto this .bat
REM    2. Or double-click and paste a path interactively
REM    3. Or:  process-game.bat "path\to\source.iso"
REM
REM  Output:  out\<friendly name> [GAMEID]\<GAMEID>.wbfs (+ .wbf1 split)
REM           Drop the whole folder into your USB HDD's wbfs\ directory.
REM ==================================================================

setlocal EnableDelayedExpansion

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

set "TOOLS=%HERE%\tools"
set "WORK=%HERE%\work"
set "OUT=%HERE%\out"
set "REGISTRY=%HERE%\disc-id-registry.txt"
set "AGREEMENT=%HERE%\LICENSE-AGREEMENT.txt"

REM ---- Pinned tool versions / download URLs -----------------------------
set "RVT_VER=2.0.1"
set "RVT_DIR=%TOOLS%\rvthtool_2.0.1-win64"
set "RVT_EXE=%RVT_DIR%\rvthtool.exe"
set "RVT_ZIP=%TOOLS%\rvthtool.zip"
set "RVT_URL=https://stuff.gerbilsoft.com/.rvt/rvthtool_2.0.1-win64.zip"

set "WIT_VER=v3.05a r8638"
set "WIT_DIR=%TOOLS%\wit-v3.05a-r8638-cygwin64"
set "WIT_EXE=%WIT_DIR%\bin\wit.exe"
set "WIT_ZIP=%TOOLS%\wit.zip"
set "WIT_URL=https://wit.wiimm.de/download/wit-v3.05a-r8638-cygwin64.zip"

set "REGISTRY_URL=https://github.com/Bird0nWheels/usbloadergx-dev-disc-support/blob/dev-disc-support/docs/CUSTOM_DISC_IDS.md"

REM ---- Banner ----------------------------------------------------------
echo.
echo  ============================================================
echo   Wii prototype-disc pipeline
echo   rvthtool retail-recryption  +  wit WBFS repack + disc-ID rename
echo  ============================================================
echo.

REM Capture the dropped path with plain SET (paren-tolerant inside
REM quotes) and test it via delayed expansion. Inlining the drop
REM argument into an IF causes cmd to tokenize the expanded value at
REM parse time, which trips "to was unexpected" on paths that contain
REM both parentheses and the word "to". Never reference the drop
REM argument from a REM either -- cmd expands percent variables
REM before skipping REM lines.
set "ARG1=%~1"
if "!ARG1!"=="" goto :prompt_src
set "SRC=!ARG1!"
goto :have_src

:prompt_src
echo Drag-and-drop the source disc image onto this window,
echo or paste its full path here.
echo.
set /p "SRC=Source path: "
REM strip surrounding quotes if present
set "SRC=!SRC:"=!"

:have_src
if not exist "!SRC!" goto :err_source_missing

REM Disk-space estimate. rvthtool extracts to a same-size retail .iso,
REM and wit packs a smaller .wbfs alongside it. Peak is ~2x source size.
for %%I in ("!SRC!") do set "SRC_SIZE=%%~zI"
set /a SRC_MB=!SRC_SIZE! / 1048576
set /a NEEDED_MB=!SRC_MB! * 2 + 200
echo.
echo Source:       !SRC!
echo Source size:  !SRC_MB! MB
echo Disk-space:   leave ^>= !NEEDED_MB! MB free on this drive while the
echo               pipeline runs (intermediate retail .iso + .wbfs).
echo               The intermediate .iso is auto-deleted after staging.
goto :after_source

:err_source_missing
echo.
echo ERROR: Source file not found:
echo   !SRC!
echo.
pause
exit /b 1

:after_source

REM ---- 2. Tools (download with one-time consent if missing) -----------
call :ensure_rvthtool || (pause & exit /b 1)
call :ensure_wit      || (pause & exit /b 1)

REM ---- 3. Ask for disc ID (with optional registry browse) --------------
:get_id
echo.
echo Choose a 6-char Wii disc ID for this build.
echo.
echo   [H] Open the local disc-ID registry in Notepad
echo   [O] Open the online registry on GitHub
echo   [Q] Cancel and quit
echo.
echo Or type your 6-char ID (e.g. RSBE3C) directly:
set /p "INPUT=Disc ID: "

if /i "!INPUT!"=="H" (
    if exist "!REGISTRY!" (
        start "" notepad "!REGISTRY!"
    ) else (
        echo Registry file missing: !REGISTRY!
    )
    goto :get_id
)
if /i "!INPUT!"=="O" (
    start "" "!REGISTRY_URL!"
    goto :get_id
)
if /i "!INPUT!"=="Q" (echo Cancelled. & pause & exit /b 1)
if "!INPUT!"=="" goto :get_id

set "GAMEID=!INPUT!"
REM Trim spaces
for /f "tokens=* delims= " %%a in ("!GAMEID!") do set "GAMEID=%%a"

REM Validate exactly 6 chars
call :strlen GAMEID GAMEID_LEN
if not "!GAMEID_LEN!"=="6" (
    echo Disc ID must be exactly 6 characters. Got !GAMEID_LEN!.
    goto :get_id
)

REM ---- 4. Look up name + title in registry, allow override -------------
set "REG_NAME="
set "REG_TITLE="
call :lookup_registry "!GAMEID!"

echo.
if not "!REG_NAME!"=="" (
    echo Registry match: !REG_NAME!  /  "!REG_TITLE!"
) else (
    echo No registry entry for !GAMEID! -- you'll be prompted for a name.
)

echo.
echo Friendly folder name. The output WBFS folder will be named:
echo   ^<your friendly name^> [!GAMEID!]
if not "!REG_NAME!"=="" echo Press Enter to use the registry default: !REG_NAME!
set /p "FRIENDLY=Friendly name: "
if "!FRIENDLY!"=="" set "FRIENDLY=!REG_NAME!"
if "!FRIENDLY!"=="" set "FRIENDLY=!GAMEID!"

echo.
echo Short title (max 30 chars). Shown on the game tile in USB Loader GX.
if not "!REG_TITLE!"=="" echo Press Enter to use the registry default: !REG_TITLE!
set /p "TITLE=Title: "
if "!TITLE!"=="" set "TITLE=!REG_TITLE!"
if "!TITLE!"=="" set "TITLE=!FRIENDLY!"

REM ---- 5. Summary + confirm -------------------------------------------
echo.
echo  ----- Pipeline summary -----
echo  Source:   !SRC!
echo  Disc ID:  !GAMEID!
echo  Folder:   !FRIENDLY! [!GAMEID!]
echo  Title:    !TITLE!
echo.
echo  Output:   !OUT!\!FRIENDLY! [!GAMEID!]\!GAMEID!.wbfs
echo  ----------------------------
echo.
set /p "GO=Press Enter to start (or 'q' to abort): "
if /i "!GO!"=="q" (echo Aborted. & pause & exit /b 1)

REM ---- 6. Run pipeline -------------------------------------------------
if not exist "!WORK!" mkdir "!WORK!"
if not exist "!OUT!" mkdir "!OUT!"

set "RETAIL_ISO=!WORK!\!GAMEID!-retail.iso"
set "TMP_WBFS=!WORK!\!GAMEID!.wbfs"
set "TMP_WBF1=!WORK!\!GAMEID!.wbf1"
set "STAGE=!OUT!\!FRIENDLY! [!GAMEID!]"

REM Stage 1: rvthtool retail extract
echo.
echo === [1/3] rvthtool retail re-encryption ===
"!RVT_EXE!" -k retail extract "!SRC!" 1 "!RETAIL_ISO!"
if errorlevel 1 (
    echo.
    echo rvthtool failed. Common causes:
    echo  - source is not a valid Wii disc image
    echo  - source is a multi-bank .rvm; try a different bank index than 1
    pause & exit /b 1
)
if not exist "!RETAIL_ISO!" (
    echo rvthtool exited cleanly but produced no output. Aborting.
    pause & exit /b 1
)

REM Stage 2: wit COPY iso -> WBFS (with directory-rebuild fallback)
echo.
echo === [2/3] wit COPY iso -^> wbfs ===
if exist "!TMP_WBFS!" del "!TMP_WBFS!"
if exist "!TMP_WBF1!" del "!TMP_WBF1!"

"!WIT_EXE!" COPY "!RETAIL_ISO!" "!TMP_WBFS!" --psel data --enc SIGN --split-size 3.5G --modify ALL --id "!GAMEID!" --name "!TITLE!" --progress --overwrite
if errorlevel 1 goto :wit_iso_failed

REM Verify the WBFS has a nonzero Directories count (catch the
REM 0-dir/0-file FST-mangled case from sparse debug builds).
REM Avoid `for /f ('""exe" args | findstr ..."')` -- the nested quotes
REM around a full-path exe trip cmd's parser with
REM "The syntax of the command is incorrect." Dump to a temp file
REM then findstr the file; clean and parser-safe.
"!WIT_EXE!" DUMP "!TMP_WBFS!" > "!WORK!\dump.txt" 2>nul
findstr /R "^  Directories: *[1-9]" "!WORK!\dump.txt" >nul
set "DUMP_OK=!ERRORLEVEL!"
del "!WORK!\dump.txt"
if not "!DUMP_OK!"=="0" goto :wit_iso_failed
echo wit COPY result has a healthy FST.
goto :wit_done

:wit_iso_failed
echo.
echo wit iso-^>wbfs path failed or produced an empty FST.
echo Falling back to directory-rebuild route (extract then rebuild)...
set "EXTRACT_DIR=!WORK!\!GAMEID!-extract"
if exist "!EXTRACT_DIR!" rmdir /s /q "!EXTRACT_DIR!"
if exist "!TMP_WBFS!" del "!TMP_WBFS!"
if exist "!TMP_WBF1!" del "!TMP_WBF1!"

"!WIT_EXE!" EXTRACT "!RETAIL_ISO!" "!EXTRACT_DIR!" --psel data --overwrite
if errorlevel 1 (echo wit EXTRACT failed too. Aborting. & pause & exit /b 1)

"!WIT_EXE!" COPY "!EXTRACT_DIR!" "!TMP_WBFS!" --enc SIGN --split-size 3.5G --modify ALL --id "!GAMEID!" --name "!TITLE!" --progress --overwrite
if errorlevel 1 (echo Directory-rebuild wit COPY failed. Aborting. & pause & exit /b 1)

rmdir /s /q "!EXTRACT_DIR!"
:wit_done

REM Stage 3: move into bracket-named stage folder
echo.
echo === [3/3] Stage into output folder ===
if exist "!STAGE!" rmdir /s /q "!STAGE!"
mkdir "!STAGE!"
REM NOTE: never put a trailing backslash inside the closing quote of a
REM path argument -- cmd parses '\"' as an escaped quote and the path
REM ends up garbled. Use the full destination filename explicitly.
move /y "!TMP_WBFS!" "!STAGE!\!GAMEID!.wbfs" >nul
if errorlevel 1 (
    echo Failed to move !GAMEID!.wbfs into !STAGE!
    pause & exit /b 1
)
if exist "!TMP_WBF1!" (
    move /y "!TMP_WBF1!" "!STAGE!\!GAMEID!.wbf1" >nul
    if errorlevel 1 (
        echo Failed to move !GAMEID!.wbf1 into !STAGE!
        pause & exit /b 1
    )
)

REM Cleanup intermediates
if exist "!RETAIL_ISO!" del "!RETAIL_ISO!"

echo.
echo  ============================================================
echo   DONE
echo  ============================================================
echo   Output folder:
echo     !STAGE!
echo.
echo   Drop that folder into the wbfs\ directory on your FAT32 USB HDD
echo   (or SD card), then launch USB Loader GX. The game will appear as:
echo     "!TITLE!"  [!GAMEID!]
echo  ============================================================
echo.
pause
exit /b 0


REM ===========================================================
REM  Subroutines
REM ===========================================================

:ensure_rvthtool
where rvthtool.exe >nul 2>&1
if not errorlevel 1 (
    for /f "delims=" %%P in ('where rvthtool.exe') do set "RVT_EXE=%%P"
    echo Using existing rvthtool on PATH: !RVT_EXE!
    exit /b 0
)
if exist "%RVT_EXE%" (
    echo Using cached rvthtool: %RVT_EXE%
    exit /b 0
)
call :tool_agreement
if errorlevel 1 exit /b 1
if not exist "%TOOLS%" mkdir "%TOOLS%"

echo.
echo Downloading rvthtool %RVT_VER% from %RVT_URL% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; try { Invoke-WebRequest -Uri '%RVT_URL%' -OutFile '%RVT_ZIP%' -UseBasicParsing -ErrorAction Stop } catch { Write-Host $_.Exception.Message; exit 1 }"
if errorlevel 1 (
    echo.
    echo Download failed. Options:
    echo  1. Install rvthtool yourself and put rvthtool.exe on PATH, or
    echo  2. Manually download %RVT_URL%
    echo     and place it at %RVT_ZIP%, then re-run this script.
    exit /b 1
)

echo Unpacking...
REM gerbilsoft's rvthtool zip is flat (no top-level dir) -- extract into
REM our own named subfolder so rvthtool's DLLs don't mingle with wit's.
REM Use [System.IO.Compression.ZipFile]::ExtractToDirectory instead of
REM Expand-Archive: the cmdlet has a known bug where it can fail
REM ("Cannot find path '...' because it does not exist") if a zip lists
REM file entries before their parent directory entries. The .NET API
REM iterates the entry table and creates directories as needed.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; if (Test-Path -LiteralPath '%RVT_DIR%') { Remove-Item -LiteralPath '%RVT_DIR%' -Recurse -Force }; New-Item -ItemType Directory -Path '%RVT_DIR%' -Force | Out-Null; try { [System.IO.Compression.ZipFile]::ExtractToDirectory('%RVT_ZIP%', '%RVT_DIR%') } catch { Write-Host $_.Exception.Message; exit 1 }"
if errorlevel 1 (echo Failed to unpack rvthtool zip. & exit /b 1)
del "%RVT_ZIP%"

if not exist "%RVT_EXE%" (
    echo Expected rvthtool.exe at %RVT_EXE% after unpack but it isn't there.
    exit /b 1
)
echo rvthtool installed at %RVT_EXE%
exit /b 0


:ensure_wit
REM PATH first
where wit.exe >nul 2>&1
if not errorlevel 1 (
    for /f "delims=" %%P in ('where wit.exe') do set "WIT_EXE=%%P"
    echo Using existing wit on PATH: !WIT_EXE!
    exit /b 0
)
REM Then our local tools dir
if exist "%WIT_EXE%" (
    echo Using cached wit: %WIT_EXE%
    exit /b 0
)

REM Need to download -- show agreement
call :tool_agreement
if errorlevel 1 exit /b 1

if not exist "%TOOLS%" mkdir "%TOOLS%"

echo.
echo Downloading wit %WIT_VER% from %WIT_URL% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; try { Invoke-WebRequest -Uri '%WIT_URL%' -OutFile '%WIT_ZIP%' -UseBasicParsing -ErrorAction Stop } catch { Write-Host $_.Exception.Message; exit 1 }"
if errorlevel 1 (
    echo.
    echo Download failed. Options:
    echo  1. Install wit yourself and put wit.exe on PATH, or
    echo  2. Manually download the wit windows zip from
    echo     https://wit.wiimm.de/
    echo     and place it at %WIT_ZIP%, then re-run this script.
    exit /b 1
)

echo Unpacking...
REM wit's zip has its own top-level dir; extract into %TOOLS% so wit's
REM dir lives alongside rvthtool's. See rvthtool unpack above for why
REM we use ZipFile.ExtractToDirectory over Expand-Archive. Clean any
REM partial extract from a previous failed run first.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; if (Test-Path -LiteralPath '%WIT_DIR%') { Remove-Item -LiteralPath '%WIT_DIR%' -Recurse -Force }; try { [System.IO.Compression.ZipFile]::ExtractToDirectory('%WIT_ZIP%', '%TOOLS%') } catch { Write-Host $_.Exception.Message; exit 1 }"
if errorlevel 1 (
    echo Failed to unpack wit zip.
    exit /b 1
)
del "%WIT_ZIP%"

if not exist "%WIT_EXE%" (
    echo Expected wit.exe at %WIT_EXE% after unpack but it isn't there.
    echo The wit zip layout may have changed.
    exit /b 1
)
echo wit installed at %WIT_EXE%
exit /b 0


:tool_agreement
REM Avoid an if/else parenthesized block here -- literal parens in any
REM echo text inside the else branch would close the block early and
REM cmd would parse the trailing words as a new command (this is the
REM "to was unexpected at this time" trap that bit us before).
REM Skip the prompt entirely if the user already accepted in this run.
if defined AGREED exit /b 0
if exist "%AGREEMENT%" goto :show_agreement_file
echo This script needs to download rvthtool and/or wit from their
echo official release URLs. Both are GPL-2-or-later.
goto :ask_consent
:show_agreement_file
type "%AGREEMENT%"
:ask_consent
echo.
set /p "AGREE=Type YES to accept and continue: "
if /i "!AGREE!"=="YES" set "AGREED=1" & exit /b 0
echo Cancelled. No download performed.
exit /b 1


:strlen
REM args: VARNAME OUT_LEN_VAR
setlocal EnableDelayedExpansion
set "STR=!%~1!"
set /a LEN=0
:strlen_loop
if not "!STR:~%LEN%,1!"=="" (
    set /a LEN+=1
    goto :strlen_loop
)
endlocal & set "%~2=%LEN%"
exit /b 0


:lookup_registry
REM arg1: GAMEID. Sets REG_NAME and REG_TITLE.
set "REG_NAME="
set "REG_TITLE="
if not exist "%REGISTRY%" exit /b 0
set "TARGET=%~1"
for /f "usebackq tokens=1,2,3 delims=|" %%A in ("%REGISTRY%") do (
    set "RAW_ID=%%A"
    REM trim spaces
    for /f "tokens=* delims= " %%X in ("!RAW_ID!") do set "TRIM_ID=%%X"
    REM trim trailing spaces
    if not "!TRIM_ID!"=="" (
        set "TRIM_ID=!TRIM_ID: =!"
        if /i "!TRIM_ID!"=="!TARGET!" (
            set "RAW_NAME=%%B"
            set "RAW_TITLE=%%C"
            REM strip leading + trailing spaces from name + title
            for /f "tokens=* delims= " %%Y in ("!RAW_NAME!") do set "TRIM_NAME=%%Y"
            for /f "tokens=* delims= " %%Z in ("!RAW_TITLE!") do set "TRIM_TITLE=%%Z"
            REM also strip trailing spaces
            call :rtrim_var TRIM_NAME
            call :rtrim_var TRIM_TITLE
            set "REG_NAME=!TRIM_NAME!"
            set "REG_TITLE=!TRIM_TITLE!"
        )
    )
)
exit /b 0


:rtrim_var
REM trim trailing spaces from the variable named in %1
setlocal EnableDelayedExpansion
set "V=!%~1!"
:rtrim_loop
if "!V:~-1!"==" " (
    set "V=!V:~0,-1!"
    goto :rtrim_loop
)
endlocal & set "%~1=%V%"
exit /b 0
