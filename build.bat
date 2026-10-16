@echo off
rem Builds bin\GlassStack.exe with the Visual Studio 2022 Build Tools (static CRT, one file, no installer).
setlocal
rem optional argument: output name (default GlassStack), so a running copy does not block a rebuild
set "OUT=%~1"
if "%OUT%"=="" set "OUT=GlassStack"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
call "%VCVARS%" >nul 2>nul || (echo vcvars64.bat not found & exit /b 1)
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
rc /nologo /fo obj\GlassStack.res src\GlassStack.rc || exit /b 1
cl /nologo /utf-8 /std:c++17 /O2 /GL /Gy /MT /EHsc /W3 /DNDEBUG /Fo:obj\ src\GlassStack.cpp obj\GlassStack.res /Fe:bin\%OUT%.exe ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
   /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll /DELAYLOAD:pdh.dll /DELAYLOAD:d3d11.dll /DELAYLOAD:dcomp.dll ^
   /DELAYLOAD:ws2_32.dll /DELAYLOAD:ole32.dll /DELAYLOAD:dwmapi.dll || exit /b 1
echo built bin\%OUT%.exe
