@echo off
chcp 936 >nul
setlocal
rem 清掉可能从父环境继承的 UTF-8 强制项，保证中文提示不乱码
set "PYTHONUTF8=0"
set "PYTHONIOENCODING="
cd /d "%~dp0"

echo ============================================
echo  ESP32 日志抓取（诊断用）
echo ============================================
echo.
echo 开始之前请先做两件事：
echo   1) 关掉 EasyCon（它占着串口，会导致抓取失败）
echo   2) 把 Switch2 打开，准备好「手柄 - 更换握把顺序」界面
echo.

set "PORT=%~1"
if "%PORT%"=="" set /p PORT=请输入串口号（例如 COM7）: 
if "%PORT%"=="" (
    echo [错误] 没有输入串口号。
    pause
    exit /b 1
)

rem ============ 自动探测带 pyserial 的 Python ============
set "PY="

rem 首选：本机已装好的隔离环境
if not defined PY (
    if exist "C:\esp32-build\pyenv\Scripts\python.exe" (
        "C:\esp32-build\pyenv\Scripts\python.exe" -c "import serial" >nul 2>nul
        if not errorlevel 1 set "PY="C:\esp32-build\pyenv\Scripts\python.exe""
    )
)

rem 其次：py -3
if not defined PY (
    py -3 -c "import serial" >nul 2>nul
    if not errorlevel 1 set "PY=py -3"
)

rem 最后：python
if not defined PY (
    python -c "import serial" >nul 2>nul
    if not errorlevel 1 set "PY=python"
)

if not defined PY (
    echo [错误] 找不到带 pyserial 的 Python。
    echo.
    echo 装一下就行：
    echo     py -3 -m pip install pyserial
    echo.
    pause
    exit /b 1
)

echo 串口号   : %PORT%
echo Python   : %PY%
echo 抓取时长 : 90 秒
echo.
echo 按任意键开始抓取。抓取期间请到 Switch2 上操作：
echo   手柄 -^> 更换握把顺序，等它搜索手柄
echo.
pause >nul

%PY% "%~dp0capture_log.py" %PORT% 90

echo.
echo [完成] 日志已保存为 diag-log.txt（就在本脚本同目录）
echo        把 diag-log.txt 发回给我即可。
echo.
pause
