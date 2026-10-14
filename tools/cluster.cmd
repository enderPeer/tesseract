@echo off
rem Runs tools/cluster.sh with Git Bash, so it works from PowerShell or cmd:
rem   tools\cluster.cmd deploy | test | scale | sweep | total [nodes]
setlocal
set "BASH=%ProgramFiles%\Git\bin\bash.exe"
if not exist "%BASH%" (
    echo error: Git Bash not found at %BASH%
    exit /b 1
)
set "SCRIPT=%~dp0cluster.sh"
"%BASH%" "%SCRIPT:\=/%" %*
