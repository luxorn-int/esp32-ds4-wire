@echo off
setlocal
cd /d "%~dp0"
if "%QT_PREFIX%"=="" (
  echo Set QT_PREFIX to your Qt installation, for example C:\Qt\6.9.1\msvc2022_64
  exit /b 1
)
if not exist build (
  cmake -S . -B build -DCMAKE_PREFIX_PATH="%QT_PREFIX%"
  if errorlevel 1 exit /b 1
)
cmake --build build --config Release
if errorlevel 1 exit /b 1
start "" "build\Release\esp32-gamepad-bridge.exe"
