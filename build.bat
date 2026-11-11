@echo off
setlocal
cd /d "%~dp0"

set SDL_VER=3.4.8
set TTF_VER=3.2.2
set SDL3=%cd%\deps\SDL3-%SDL_VER%
set TTF=%cd%\deps\SDL3_ttf-%TTF_VER%

rem ---------- 1. download dependencies (only the first time) ----------
if not exist deps mkdir deps

if not exist "%SDL3%\include" (
  echo Downloading SDL3 %SDL_VER% ...
  powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest 'https://github.com/libsdl-org/SDL/releases/download/release-%SDL_VER%/SDL3-devel-%SDL_VER%-VC.zip' -OutFile 'deps\sdl3.zip'; Expand-Archive 'deps\sdl3.zip' -DestinationPath 'deps' -Force"
  if errorlevel 1 goto fail
  del deps\sdl3.zip
)

if not exist "%TTF%\include" (
  echo Downloading SDL3_ttf %TTF_VER% ...
  powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest 'https://github.com/libsdl-org/SDL_ttf/releases/download/release-%TTF_VER%/SDL3_ttf-devel-%TTF_VER%-VC.zip' -OutFile 'deps\ttf.zip'; Expand-Archive 'deps\ttf.zip' -DestinationPath 'deps' -Force"
  if errorlevel 1 goto fail
  del deps\ttf.zip
)

if not exist stb_image_write.h (
  echo Downloading stb_image_write.h ...
  powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest 'https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h' -OutFile 'stb_image_write.h'"
  if errorlevel 1 goto fail
)

if not exist "%SDL3%\include\SDL3\SDL.h" goto fail
if not exist "%TTF%\include\SDL3_ttf\SDL_ttf.h" goto fail

rem ---------- 2. find the Visual Studio compiler if this is a normal terminal ----------
where cl >nul 2>nul
if not errorlevel 1 goto havecl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto fail

:havecl
rem ---------- 3. app icon: put icon.png / icon.jpg / icon.ico in the "icon" folder ----------
del app.rc app.res app_icon.ico SnapTranslate.ico 2>nul
set "RESFILE="
set "ICONSRC="
if exist icon\icon.bmp  set "ICONSRC=icon\icon.bmp"
if exist icon\icon.jpeg set "ICONSRC=icon\icon.jpeg"
if exist icon\icon.jpg  set "ICONSRC=icon\icon.jpg"
if exist icon\icon.png  set "ICONSRC=icon\icon.png"
if exist icon\icon.ico  set "ICONSRC=icon\icon.ico"
if "%ICONSRC%"=="" goto noicon

set "ICOFILE=%ICONSRC%"
if /i "%ICONSRC:~-4%"==".ico" goto haveico
if not exist make_icon.ps1 goto noscript
echo [icon] Converting %ICONSRC% ...
powershell -NoProfile -ExecutionPolicy Bypass -File make_icon.ps1 -Src "%ICONSRC%" -Dst "app_icon.ico"
if errorlevel 1 goto iconfail
if not exist app_icon.ico goto iconfail
set "ICOFILE=app_icon.ico"

:haveico
> app.rc echo 1 ICON "%ICOFILE:\=/%"
where rc >nul 2>nul
if errorlevel 1 goto norc
rc /nologo /fo app.res app.rc
if errorlevel 1 goto iconfail
if not exist app.res goto iconfail
set "RESFILE=app.res"
copy /y "%ICOFILE%" SnapTranslate.ico >nul
echo [icon] OK - embedded %ICOFILE%
goto icondone

:noscript
echo [icon] make_icon.ps1 is missing from this folder - copy it next to build.bat.
goto iconfail
:norc
echo [icon] rc.exe not found - in the Visual Studio installer add the "Windows 11 SDK" component.
:iconfail
echo [icon] FAILED - the exe will use the default Windows icon. Fix the message above and run build.bat again.
goto icondone
:noicon
echo [icon] No icon found - put icon.png / icon.jpg / icon.ico in the "icon" folder next to build.bat.
:icondone

rem ---------- 4. build ----------
cl /nologo /std:c++17 /permissive- /O2 /MT /EHsc /utf-8 /I"%SDL3%\include" /I"%TTF%\include" main.cpp ^
   /link /SUBSYSTEM:WINDOWS /LIBPATH:"%SDL3%\lib\x64" /LIBPATH:"%TTF%\lib\x64" ^
   SDL3.lib SDL3_ttf.lib winhttp.lib user32.lib gdi32.lib dwmapi.lib shell32.lib advapi32.lib windowsapp.lib %RESFILE% /OUT:SnapTranslate.exe
if errorlevel 1 goto fail

copy /y "%SDL3%\lib\x64\SDL3.dll" . >nul
copy /y "%TTF%\lib\x64\SDL3_ttf.dll" . >nul
echo.
echo Done: SnapTranslate.exe
exit /b 0

:novs
echo Visual Studio C++ tools not found. Install "Build Tools for Visual Studio" with the "Desktop development with C++" workload.
exit /b 1

:fail
echo.
echo Build failed - see the messages above.
exit /b 1
