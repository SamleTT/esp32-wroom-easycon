@echo off
chcp 936 >nul
setlocal
rem 清掉可能从父环境继承的 UTF-8 强制项，保证 esptool 中文提示与 GBK 控制台一致
set "PYTHONUTF8=0"
set "PYTHONIOENCODING="
cd /d "%~dp0"

echo ============================================
echo  OpenSwitchPad 固件烧录 (三个分区分别写入)
echo ============================================
echo.

set "PORT=%~1"
if "%PORT%"=="" set /p PORT=请输入串口号 (例如 COM3): 
if "%PORT%"=="" (
    echo [错误] 没有输入串口号。
    pause
    exit /b 1
)

for %%F in (bootloader.bin partitions_singleapp.bin OpenSwitchPad.bin) do (
    if not exist "%%F" (
        echo [错误] 找不到 %%F
        pause
        exit /b 1
    )
)

rem ============ 自动探测可用的 esptool（按优先级） ============
set "ESPTOOL="

rem 候选 1：本机已装好的隔离环境（无需任何安装，优先）
if not defined ESPTOOL (
    if exist "C:\esp32-build\pyenv\Scripts\python.exe" (
        "C:\esp32-build\pyenv\Scripts\python.exe" -c "import esptool" >nul 2>nul
        if not errorlevel 1 set "ESPTOOL="C:\esp32-build\pyenv\Scripts\python.exe" -m esptool"
    )
)

rem 候选 2：PATH 上的 esptool.exe
if not defined ESPTOOL (
    where esptool >nul 2>nul
    if not errorlevel 1 set "ESPTOOL=esptool"
)

rem 候选 3：PATH 上的 esptool.py
if not defined ESPTOOL (
    where esptool.py >nul 2>nul
    if not errorlevel 1 set "ESPTOOL=esptool.py"
)

rem 候选 4：py -3 -m esptool
if not defined ESPTOOL (
    py -3 -c "import esptool" >nul 2>nul
    if not errorlevel 1 set "ESPTOOL=py -3 -m esptool"
)

rem 候选 5：python -m esptool
if not defined ESPTOOL (
    python -c "import esptool" >nul 2>nul
    if not errorlevel 1 set "ESPTOOL=python -m esptool"
)

if not defined ESPTOOL (
    echo [错误] 本机没找到可用的 esptool。
    echo.
    echo 任选一种方式安装，然后在同一窗口重开本脚本：
    echo     py -3 -m pip install esptool
    echo     python -m pip install esptool
    echo.
    echo 也可以不用本脚本，直接手敲下面这一行：
    echo.
    echo "C:\esp32-build\pyenv\Scripts\python.exe" -m esptool --chip esp32 --port COM3 --baud 921600 write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect 0x1000 "%~dp0bootloader.bin" 0x8000 "%~dp0partitions_singleapp.bin" 0x10000 "%~dp0OpenSwitchPad.bin"
    echo.
    pause
    exit /b 1
)

echo 目标串口 : %PORT%
echo 烧录工具 : %ESPTOOL%
echo   0x1000  bootloader.bin
echo   0x8000  partitions_singleapp.bin
echo   0x10000 OpenSwitchPad.bin
echo.
echo 开始烧录，请勿断电...
echo.

%ESPTOOL% --chip esp32 --port %PORT% --baud 921600 ^
  --before default_reset --after hard_reset ^
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect ^
  0x1000 bootloader.bin 0x8000 partitions_singleapp.bin 0x10000 OpenSwitchPad.bin

if errorlevel 1 (
    echo.
    echo [失败] 烧录出错。常见原因：
    echo        1. 串口被其它程序占用（关掉串口助手 / EasyCon / 串口监视器）
    echo        2. COM 口填错
    echo        3. 需要手动进下载模式：先按住 BOOT 键，再点一下 EN 键
    pause
    exit /b 1
)

echo.
echo [成功] 烧录完成。按复位键或重新上电即可。
echo.
pause
