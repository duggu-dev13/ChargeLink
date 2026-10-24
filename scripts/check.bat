@echo off
rem Build everything and run every test. Run from H:\chargelink in the x64 developer prompt.
if "%CHARGELINK_TEST_DB%"=="" echo WARNING: CHARGELINK_TEST_DB is not set - the database tests will be SKIPPED
cmake --build --preset default || exit /b 1
.\build\chargelink_tests.exe || exit /b 1
echo.
echo CHECK PASSED