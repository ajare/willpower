@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "WITH_MPP_LFS=false"
set "BUILD_TYPE=Release"
set "BUILD_DIR=build"

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="--with-mpp-lfs" (
    set "WITH_MPP_LFS=true"
    shift
    goto parse_args
)
if /i "%~1"=="--build-type" (
    if "%~2"=="" (
        set "ERROR_MESSAGE=--build-type requires a value"
        goto fatal
    )
    set "BUILD_TYPE=%~2"
    shift
    shift
    goto parse_args
)
if /i "%~1"=="--build-dir" (
    if "%~2"=="" (
        set "ERROR_MESSAGE=--build-dir requires a value"
        goto fatal
    )
    set "BUILD_DIR=%~2"
    shift
    shift
    goto parse_args
)
if /i "%~1"=="-h" goto usage_success
if /i "%~1"=="--help" goto usage_success
set "ERROR_MESSAGE=unknown option: %~1 (run with --help for usage)"
goto fatal

:args_done
where git >nul 2>&1
if errorlevel 1 (
    set "ERROR_MESSAGE=Git is required but was not found on PATH"
    goto fatal
)
where cmake >nul 2>&1
if errorlevel 1 (
    set "ERROR_MESSAGE=CMake is required but was not found on PATH"
    goto fatal
)

for %%I in ("%~dp0.") do set "ROOT_DIR=%%~fI"
pushd "%ROOT_DIR%"
if errorlevel 1 (
    set "ERROR_MESSAGE=could not enter repository directory: %ROOT_DIR%"
    goto fatal
)

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
    set "ERROR_MESSAGE=%ROOT_DIR% is not a Git checkout"
    goto fatal
)
if not exist "CMakeLists.txt" (
    set "ERROR_MESSAGE=run this script from the Willpower checkout"
    goto fatal
)
if not exist ".gitmodules" (
    set "ERROR_MESSAGE=run this script from the Willpower checkout"
    goto fatal
)

if /i "%WITH_MPP_LFS%"=="true" (
    git lfs version >nul 2>&1
    if errorlevel 1 goto missing_lfs
)

echo Synchronizing and checking out all submodules...
git submodule sync --recursive
if errorlevel 1 (
    set "ERROR_MESSAGE=failed to synchronize submodules"
    goto fatal
)
git submodule update --init --recursive
if errorlevel 1 (
    set "ERROR_MESSAGE=failed to check out submodules"
    goto fatal
)

set "SUBMODULE_STATUS_FILE=%TEMP%\willpower-submodules-%RANDOM%-%RANDOM%.txt"
git submodule status --recursive > "%SUBMODULE_STATUS_FILE%"
if errorlevel 1 (
    set "ERROR_MESSAGE=failed to inspect submodules"
    goto fatal
)
type "%SUBMODULE_STATUS_FILE%"
findstr /r /b /c:"[+U-]" "%SUBMODULE_STATUS_FILE%" >nul
if not errorlevel 1 (
    del /q "%SUBMODULE_STATUS_FILE%" >nul 2>&1
    set "ERROR_MESSAGE=one or more submodules are not checked out at the commits recorded by their parent"
    goto fatal
)
del /q "%SUBMODULE_STATUS_FILE%" >nul 2>&1

set "MPP_DIR=%ROOT_DIR%\ext\massive-poly-pusher"
if not exist "%MPP_DIR%\CMakeLists.txt" (
    set "ERROR_MESSAGE=MassivePolyPusher was not checked out correctly"
    goto fatal
)

if /i "%WITH_MPP_LFS%"=="true" (
    echo Downloading MassivePolyPusher Git LFS files...
    git -C "%MPP_DIR%" lfs install --local
    if errorlevel 1 (
        set "ERROR_MESSAGE=failed to initialize Git LFS for MassivePolyPusher"
        goto fatal
    )
    git -C "%MPP_DIR%" lfs pull
    if errorlevel 1 (
        set "ERROR_MESSAGE=failed to download MassivePolyPusher Git LFS files"
        goto fatal
    )
)

if not defined BUILD_DIR (
    set "ERROR_MESSAGE=refusing to remove an empty build directory"
    goto fatal
)
for %%I in ("%BUILD_DIR%") do set "BUILD_DIR=%%~fI"
if /i "%BUILD_DIR%"=="%ROOT_DIR%" (
    set "ERROR_MESSAGE=refusing to remove unsafe build directory: %BUILD_DIR%"
    goto fatal
)
for %%I in ("%BUILD_DIR%\..") do set "BUILD_PARENT=%%~fI"
if /i "%BUILD_DIR%"=="%BUILD_PARENT%" (
    set "ERROR_MESSAGE=refusing to remove unsafe build directory: %BUILD_DIR%"
    goto fatal
)

echo Removing previous build output...
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if exist "%BUILD_DIR%" (
    set "ERROR_MESSAGE=could not remove build directory: %BUILD_DIR%"
    goto fatal
)
if exist "%MPP_DIR%\build" rmdir /s /q "%MPP_DIR%\build"
if exist "%MPP_DIR%\build" (
    set "ERROR_MESSAGE=could not remove dependency build directory: %MPP_DIR%\build"
    goto fatal
)

echo Configuring %BUILD_TYPE% build in %BUILD_DIR%...
cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE="%BUILD_TYPE%"
if errorlevel 1 (
    set "ERROR_MESSAGE=CMake configuration failed"
    goto fatal
)

echo Building Willpower and dependencies...
cmake --build "%BUILD_DIR%" --config "%BUILD_TYPE%" --parallel
if errorlevel 1 (
    set "ERROR_MESSAGE=build failed"
    goto fatal
)

echo Build completed successfully.
popd
exit /b 0

:usage_success
call :usage
exit /b 0

:missing_lfs
>&2 echo error: --with-mpp-lfs requires Git LFS, but 'git lfs' is not installed.
>&2 echo.
>&2 echo Install Git LFS, then run this script again:
>&2 echo   winget install GitHub.GitLFS
>&2 echo.
>&2 echo For other installation methods, see https://git-lfs.com/.
exit /b 1

:usage
echo Usage: build_from_scratch.bat [options]
echo.
echo Configure and build Willpower and its required dependencies from scratch.
echo.
echo Options:
echo   --with-mpp-lfs       Download MassivePolyPusher's Git LFS files.
echo   --build-type TYPE    CMake build type ^(default: Release^).
echo   --build-dir DIR      Build directory, relative to the repository root unless
echo                        absolute ^(default: build^).
echo   -h, --help           Show this help.
echo.
echo Environment:
echo   CC, CXX               Select the C and C++ compilers during configuration.
echo   CMAKE_GENERATOR       Select a CMake generator.
echo   CMAKE_BUILD_PARALLEL_LEVEL
echo                         Limit the number of parallel build jobs.
exit /b 0

:fatal
>&2 <nul set /p "=error: %ERROR_MESSAGE%"
>&2 echo.
exit /b 1
