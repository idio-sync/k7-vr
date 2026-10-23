@echo off
rem Configures and builds the 32-bit probe into out\ with MSVC (VS 18 Build Tools, x86) and C:\ninja.exe.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out\build.ninja (
    cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_MAKE_PROGRAM=C:/ninja.exe -DCMAKE_CXX_COMPILER=cl || exit /b 1
)
cmake --build out || exit /b 1
echo built out\XINPUT1_3.dll
