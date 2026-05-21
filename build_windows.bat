@echo off
setlocal

set "VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"

if not exist "%VSDEVCMD%" goto missing_vs

call "%VSDEVCMD%" -arch=x64
if errorlevel 1 exit /b %errorlevel%

cmake -S . -B build-vs2022 -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b %errorlevel%

cmake --build build-vs2022 --config Debug
if errorlevel 1 exit /b %errorlevel%

build-vs2022\Debug\large_sdf_tests.exe
if errorlevel 1 exit /b %errorlevel%

build-vs2022\Debug\large_sdf_demo.exe
if errorlevel 1 exit /b %errorlevel%

echo.
echo OK - OBJ ecrits dans out\sdf_demo_blocky.obj et out\sdf_demo_smooth.obj
exit /b 0

:missing_vs
echo Visual Studio Build Tools 2022 introuvable.
exit /b 1
