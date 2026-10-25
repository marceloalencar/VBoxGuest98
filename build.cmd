@echo off
rem Builds all components with Visual C++ 6.0 and the Windows 2000 DDK.
rem
rem Usage: build.cmd [free|checked]
rem
rem Override the tool locations with the MSVC6DIR and DDKDIR environment
rem variables (defaults: C:\MSVC600 and C:\NTDDK).

setlocal
if "%MSVC6DIR%"=="" set MSVC6DIR=C:\MSVC600
if "%DDKDIR%"=="" set DDKDIR=C:\NTDDK
set BUILDTYPE=%1
if "%BUILDTYPE%"=="" set BUILDTYPE=free

if not exist "%MSVC6DIR%\VC98\Bin\VCVARS32.BAT" goto no_msvc
if not exist "%DDKDIR%\bin\setenv.bat" goto no_ddk

call "%MSVC6DIR%\VC98\Bin\VCVARS32.BAT" >nul
rem setenv.bat runs %MSDEVDIR%\bin\vcvars32, which lives in VC98\Bin.
set MSDevDir=%MSVC6DIR%\VC98
call "%DDKDIR%\bin\setenv.bat" %DDKDIR% %BUILDTYPE%
cd /d "%~dp0"
build -cZ
goto end

:no_msvc
echo Visual C++ 6.0 not found in %MSVC6DIR% (set MSVC6DIR).
exit /b 1

:no_ddk
echo Windows 2000 DDK not found in %DDKDIR% (set DDKDIR).
exit /b 1

:end
endlocal
