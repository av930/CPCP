@echo off
setlocal EnableExtensions
rem Publishes a ClipMaster release: installs GitHub CLI if needed, logs in if needed, then runs Publish-Release.ps1.
rem Extra arguments are passed on, for example: Publish-Release.cmd -Version 2026.11.20-2 -SkipTests

call :find_gh
if not defined GH_DIR (
	echo [gh] GitHub CLI was not found. Installing it...
	powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-GitHubCli.ps1"
	if errorlevel 1 goto :fail
	call :find_gh
)
if not defined GH_DIR (
	echo [gh] GitHub CLI is still not available after the installation.
	goto :fail
)
set "PATH=%GH_DIR%;%PATH%"

gh.exe auth status --hostname github.com >nul 2>&1
if errorlevel 1 (
	echo [gh] Not logged in to github.com. Starting gh auth login...
	gh.exe auth login --hostname github.com --git-protocol https --web
	if errorlevel 1 goto :fail
)

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Publish-Release.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
goto :done

:fail
set "EXIT_CODE=1"

:done
echo.
pause
exit /b %EXIT_CODE%

:find_gh
set "GH_DIR="
for /f "delims=" %%I in ('where gh.exe 2^>nul') do if not defined GH_DIR set "GH_DIR=%%~dpI"
if defined GH_DIR goto :eof
for %%D in ("%ProgramFiles%\GitHub CLI" "%LOCALAPPDATA%\Programs\GitHub CLI\bin") do (
	if not defined GH_DIR if exist "%%~D\gh.exe" set "GH_DIR=%%~D"
)
goto :eof
