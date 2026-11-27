@echo off
rem Removes the build\ directory.

cd /d "%~dp0"
if exist build rmdir /s /q build
echo removed build
