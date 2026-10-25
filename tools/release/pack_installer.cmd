@echo off
rem ============================================================
rem  hotrainy - build the SINGLE-FILE INSTALLER (installer-only)
rem  Output: hotrainy_<ver>_installer.exe  +  hotrainy_<ver>_setup.zip
rem  Just double-click this file after changing the program.
rem ============================================================
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "SRC=%ROOT%\cpp\hotrainy\tools\installer\hr_installer.cpp"
set "BUILD=%ROOT%\cpp\hotrainy\tools\installer\build"
set "PY=D:\dsh\resources\runtime\primary-runtime\dependencies\python\python.exe"

if not exist "%BUILD%" mkdir "%BUILD%"
echo [1/3] compiling installer stub ...
call "%BT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
pushd "%BUILD%"
cl /nologo /utf-8 /MT /O2 /EHsc /DUNICODE /D_UNICODE /W3 /Fe:hr_installer_stub.exe "%SRC%" /link /SUBSYSTEM:WINDOWS shell32.lib shlwapi.lib ole32.lib advapi32.lib user32.lib gdi32.lib > build.log 2>&1
set CLERR=%errorlevel%
popd
if not "%CLERR%"=="0" (
    echo [FAIL] stub compile failed - see %BUILD%\build.log
    echo.
    type "%BUILD%\build.log"
    pause
    exit /b 1
)

echo [2/3] packing payload ...
"%PY%" "%ROOT%\pack_installer.py"
if errorlevel 1 ( echo [FAIL] packing failed & pause & exit /b 1 )

echo [3/3] zipping ...
powershell -NoProfile -Command "$e='%ROOT%\hotrainy_installer.exe'; if (Test-Path $e) { $z='%ROOT%\hotrainy_setup.zip'; if (Test-Path $z) { Remove-Item $z -Force }; Compress-Archive -Path $e -DestinationPath $z -Force; Copy-Item $z ([Environment]::GetFolderPath('Desktop')) -Force; Write-Host ('  zip: ' + $z) } else { Write-Host '  install exe not found' }"

echo.
echo Done. Deliverable:
echo   %ROOT%\hotrainy_setup.zip   (contains the single installer exe)
echo.
pause