@echo off
rem Builds all components with Visual C++ 6.0 and the Windows 2000 DDK (the
rem VxD with the Windows 98 DDK's assembler and linker), then the installer
rem with NSIS 2.46 when it is available.
rem
rem Usage: build.cmd [free|checked]
rem
rem Override the tool locations with the MSVC6DIR, DDKDIR, DDK98DIR and
rem NSIS2DIR environment variables (defaults: C:\MSVC600, C:\NTDDK, C:\98DDK
rem and C:\NSIS246).

setlocal
if "%MSVC6DIR%"=="" set MSVC6DIR=C:\MSVC600
if "%DDKDIR%"=="" set DDKDIR=C:\NTDDK
if "%DDK98DIR%"=="" set DDK98DIR=C:\98DDK
if "%NSIS2DIR%"=="" set NSIS2DIR=C:\NSIS246
set BUILDTYPE=%1
if "%BUILDTYPE%"=="" set BUILDTYPE=free
set OBJDIR=objfre
if /i "%BUILDTYPE%"=="checked" set OBJDIR=objchk

if not exist "%MSVC6DIR%\VC98\Bin\VCVARS32.BAT" goto no_msvc
if not exist "%DDKDIR%\bin\setenv.bat" goto no_ddk
if not exist "%DDK98DIR%\bin\win98\ml.exe" goto no_ddk98

call "%MSVC6DIR%\VC98\Bin\VCVARS32.BAT" >nul
rem setenv.bat runs %MSDEVDIR%\bin\vcvars32, which lives in VC98\Bin.
set MSDevDir=%MSVC6DIR%\VC98
call "%DDKDIR%\bin\setenv.bat" %DDKDIR% %BUILDTYPE%
cd /d "%~dp0"
build -cZ

rem VBoxMou.vxd: build.exe has no VxD support, so assemble and link it here.
if not exist VBoxMou\%OBJDIR%\i386 mkdir VBoxMou\%OBJDIR%\i386
"%DDK98DIR%\bin\win98\ml.exe" -nologo -coff -DBLD_COFF -DIS_32 -DMASM6 -DDEBLEVEL=0 -W2 -c -Cx -Zm ^
    -I"%DDK98DIR%\inc\win98" -FoVBoxMou\%OBJDIR%\i386\VBoxMou.obj VBoxMou\VBoxMou.asm
if errorlevel 1 goto failed
"%DDK98DIR%\bin\link.exe" -nologo -vxd -nod -ignore:4078 -ignore:4039 -def:VBoxMou\VBoxMou.def ^
    -out:VBoxMou\%OBJDIR%\i386\VBoxMou.vxd -map:VBoxMou\%OBJDIR%\i386\VBoxMou.map VBoxMou\%OBJDIR%\i386\VBoxMou.obj
if errorlevel 1 goto failed

if not exist "%NSIS2DIR%\makensis.exe" goto no_nsis
"%NSIS2DIR%\makensis.exe" /V2 /DOBJDIR=%OBJDIR% installer\VBoxGuest98.nsi
goto end

:no_nsis
echo NSIS 2.46 not found in %NSIS2DIR% (set NSIS2DIR); installer not built.
goto end

:no_msvc
echo Visual C++ 6.0 not found in %MSVC6DIR% (set MSVC6DIR).
exit /b 1

:no_ddk
echo Windows 2000 DDK not found in %DDKDIR% (set DDKDIR).
exit /b 1

:no_ddk98
echo Windows 98 DDK not found in %DDK98DIR% (set DDK98DIR).
exit /b 1

:failed
echo VBoxMou.vxd build failed.
exit /b 1

:end
endlocal
