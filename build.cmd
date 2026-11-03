@echo off
rem Builds dinput8.dll with MSVC + Ninja into build\. "build.cmd dist" also packages dist\kcd-clarity-<version>.zip.
rem "build.cmd gog [dist]" builds against the GOG WHGame.dll symbols (zip: kcd-clarity-<version>-gog.zip).
rem Needs Visual Studio 2022 (or Build Tools) with the C++ x64 tools; CMake and Ninja come with it.
setlocal
set "ROOT=%~dp0"
set "GOG=OFF"
set "SUFFIX="
if /i "%~1"=="gog" (set "GOG=ON" & set "SUFFIX=-gog" & shift)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo vswhere found no Visual Studio with the C++ x64 tools
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%ROOT%." -B "%ROOT%build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKCD_GOG=%GOG% || exit /b 1
cmake --build "%ROOT%build" || exit /b 1
if /i not "%~1"=="dist" exit /b 0

for /f "tokens=2 delims=()" %%v in ('findstr /r /c:"^project(" "%ROOT%CMakeLists.txt"') do set "PROJ=%%v"
for /f "tokens=3" %%v in ("%PROJ%") do set "VER=%%v"
set "STAGE=%ROOT%build\dist\kcd-clarity"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\kcd_clarity_licenses" || exit /b 1
copy /y "%ROOT%build\dinput8.dll" "%STAGE%\" >nul || exit /b 1
copy /y "%ROOT%build\ngx\rel\nvngx_dlss.dll" "%STAGE%\" >nul || exit /b 1
copy /y "%ROOT%release\README.txt" "%STAGE%\kcd_clarity_readme.txt" >nul || exit /b 1
set "LIC=%STAGE%\kcd_clarity_licenses"
copy /y "%ROOT%LICENSE" "%LIC%\kcd-clarity.txt" >nul || exit /b 1
copy /y "%ROOT%build\_deps\imgui-src\LICENSE.txt" "%LIC%\dear-imgui.txt" >nul || exit /b 1
copy /y "%ROOT%build\_deps\minhook-src\LICENSE.txt" "%LIC%\minhook.txt" >nul || exit /b 1
for /d %%d in ("%ROOT%build\dlss-sdk-*") do copy /y "%%d\LICENSE.txt" "%LIC%\nvidia-dlss-sdk.txt" >nul || exit /b 1
if not exist "%ROOT%dist" mkdir "%ROOT%dist"
set "ZIP=%ROOT%dist\kcd-clarity-%VER%%SUFFIX%.zip"
if exist "%ZIP%" del "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%'" || exit /b 1
echo %ZIP%
