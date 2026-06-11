@echo off
REM ============================================================
REM  Turbine Monitor 微服务启动脚本 (Windows)
REM  启动顺序: pxi_collector -> cavitation_detector ->
REM            fatigue_evaluator -> alarm_pusher -> api_gateway
REM ============================================================

setlocal
cd /d "%~dp0"

set BACKEND_DIR=%cd%\backend
set BUILD_DIR=%BACKEND_DIR%\build\bin
set CONFIG_FILE=%cd%\config\config.json

echo ========================================
echo   Turbine Monitor 微服务启动器
echo ========================================
echo.

if not exist "%BUILD_DIR%" (
    echo [ERROR] 未找到编译输出目录: %BUILD_DIR%
    echo 请先执行 cmake --build .\build --config Release
    pause
    exit /b 1
)

REM 清理旧的共享内存文件
echo [INIT] 清理共享内存残留...
del /q %TEMP%\shm_turbine_* 2>nul

REM 启动各微服务 (使用 START 每个服务在独立窗口中运行)
echo [START] Launching PXI Collector (Service 1/5)...
start "PXI Collector - 高速UDP采集" "%BUILD_DIR%\pxi_collector\pxi_collector.exe" -c "%CONFIG_FILE%"
timeout /t 2 >nul

echo [START] Launching Cavitation Detector (Service 2/5)...
start "Cavitation Detector - 空化识别" "%BUILD_DIR%\cavitation_detector\cavitation_detector.exe" -c "%CONFIG_FILE%"
timeout /t 2 >nul

echo [START] Launching Fatigue Evaluator (Service 3/5)...
start "Fatigue Evaluator - 寿命评估" "%BUILD_DIR%\fatigue_evaluator\fatigue_evaluator.exe" -c "%CONFIG_FILE%"
timeout /t 2 >nul

echo [START] Launching Alarm Pusher (Service 4/5)...
start "Alarm Pusher - 告警IEC61850" "%BUILD_DIR%\alarm_pusher\alarm_pusher.exe" -c "%CONFIG_FILE%"
timeout /t 2 >nul

echo [START] Launching API Gateway (Service 5/5)...
start "API Gateway - REST聚合" "%BUILD_DIR%\api_gateway\api_gateway.exe" -c "%CONFIG_FILE%"
timeout /t 2 >nul

echo.
echo ========================================
echo   所有服务已启动！
echo ========================================
echo   IPC拓扑:
echo     RAW(0)   : pxi_collector -> cavitation_detector / fatigue_evaluator
echo     FEATURES(1) : pxi_collector -> cavitation_detector
echo     CAVITATION(2): cavitation_detector -> fatigue_evaluator / alarm_pusher / api_gateway
echo     STRESS(3) : fatigue_evaluator -> alarm_pusher
echo     LIFE(4)   : fatigue_evaluator -> alarm_pusher / api_gateway
echo     ALARM(5)  : alarm_pusher -> api_gateway
echo ========================================
echo.
echo 启动模拟器? (Y/N)
set /p START_SIM=
if /i "%START_SIM%"=="Y" (
    echo [START] Launching PXI Simulator...
    start "PXI Sim - UDP流量" "%BUILD_DIR%\pxi_simulator.exe"
)

echo.
echo 按任意键停止所有服务...
pause >nul

echo [STOP] Stopping all services...
taskkill /F /IM pxi_collector.exe >nul 2>&1
taskkill /F /IM cavitation_detector.exe >nul 2>&1
taskkill /F /IM fatigue_evaluator.exe >nul 2>&1
taskkill /F /IM alarm_pusher.exe >nul 2>&1
taskkill /F /IM api_gateway.exe >nul 2>&1
taskkill /F /IM pxi_simulator.exe >nul 2>&1
del /q %TEMP%\shm_turbine_* 2>nul
echo [DONE] 所有服务已停止
endlocal
