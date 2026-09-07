@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "CONFIG=Release"

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="/config" (
    if "%~2"=="" (
        set "ERROR_MESSAGE=/config requires a value"
        goto fatal
    )
    set "CONFIG=%~2"
    shift
    shift
    goto parse_args
)
if /i "%~1"=="/?" goto usage_success
if /i "%~1"=="/help" goto usage_success
set "ERROR_MESSAGE=unknown option: %~1 (run with /? for usage)"
goto fatal

:args_done
for %%I in ("%~dp0.") do set "ROOT_DIR=%%~fI"
cmake -S "%ROOT_DIR%" -B "%ROOT_DIR%\build-windows" -DCMAKE_BUILD_TYPE="%CONFIG%"
exit /b %ERRORLEVEL%

:usage_success
call :usage
exit /b 0

:usage
echo Usage: regenerate_build.bat [/config CONFIG]
echo.
echo Regenerate the Willpower build system with CMake.
echo.
echo Options:
echo   /config CONFIG    CMake build configuration ^(default: Release^).
echo(  /?, /help         Show this help.
exit /b 0

:fatal
>&2 echo error: %ERROR_MESSAGE%
exit /b 1
