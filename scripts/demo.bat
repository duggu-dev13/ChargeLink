@echo off
rem Starts the database, the CSMS and two simulators in separate windows.
rem Needs CHARGELINK_DB to be set in this window first.
if "%CHARGELINK_DB%"=="" (
  echo Set CHARGELINK_DB first, see README.md
  exit /b 2
)
docker compose up -d || exit /b 1
start "CSMS" cmd /k ".\build\csms.exe -v"
timeout /t 3 /nobreak >nul
start "CP001" cmd /k ".\build\simulator.exe 127.0.0.1 9000 CP001"
start "CP002" cmd /k ".\build\simulator.exe 127.0.0.1 9000 CP002"
timeout /t 3 /nobreak >nul
.\build\report.exe chargers
echo.
echo In the CP001 window try:  plug 16   then   tap   then (after ~10 s)   tap
echo Then run:  .\build\report.exe transactions   and   .\build\report.exe energy