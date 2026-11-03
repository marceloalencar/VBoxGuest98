@echo off
rem Builds all components with Visual C++ 6.0 and the Windows 98 DDK, then the
rem installer with NSIS 2.46 and, for the free build, its floppy image with
rem Python 3 when they are available.
rem
rem Usage: build.cmd [free|checked]
rem
rem Override the tool locations with the MSVC6DIR, DDK98DIR, NSIS2DIR and
rem PYTHON environment variables (defaults: C:\MSVC600, C:\98DDK, C:\NSIS246
rem and python from the PATH).

setlocal
if "%MSVC6DIR%"=="" set MSVC6DIR=C:\MSVC600
if "%DDK98DIR%"=="" set DDK98DIR=C:\98DDK
if "%NSIS2DIR%"=="" set NSIS2DIR=C:\NSIS246
if "%PYTHON%"=="" set PYTHON=python
set BUILDTYPE=%1
if "%BUILDTYPE%"=="" set BUILDTYPE=free
if /i not "%BUILDTYPE%"=="free" if /i not "%BUILDTYPE%"=="checked" goto usage
rem Where the Windows 98 DDK puts the targets: obj\i386\free or obj\i386\checked.
set OBJDIR=obj\i386\%BUILDTYPE%

if not exist "%MSVC6DIR%\VC98\Bin\VCVARS32.BAT" goto no_msvc
if not exist "%DDK98DIR%\bin\setenv.bat" goto no_ddk98

call "%MSVC6DIR%\VC98\Bin\VCVARS32.BAT" >nul
rem setenv.bat runs %MSDEVDIR%\bin\vcvars32, which lives in VC98\Bin.
set MSDevDir=%MSVC6DIR%\VC98
call "%DDK98DIR%\bin\setenv.bat" %DDK98DIR% %BUILDTYPE%
cd /d "%~dp0"
rem build.exe does not create the target folders and the link fails without them.
for %%d in (VBoxGst VBoxTray VBoxControl VBoxNP VBoxMou VBoxSF) do if not exist %%d\%OBJDIR% mkdir %%d\%OBJDIR%
rem build.exe by name: plain "build" finds this build.cmd in the current folder first.
build.exe -cZ

rem VBoxMou.vxd: build.exe has no VxD support, so assemble and link it here.
"%DDK98DIR%\bin\win98\ml.exe" -nologo -coff -DBLD_COFF -DIS_32 -DMASM6 -DDEBLEVEL=0 -W2 -c -Cx -Zm ^
    -I"%DDK98DIR%\inc\win98" -FoVBoxMou\%OBJDIR%\VBoxMou.obj VBoxMou\VBoxMou.asm
if errorlevel 1 goto failed
"%DDK98DIR%\bin\link.exe" -nologo -vxd -nod -ignore:4078 -ignore:4039 -def:VBoxMou\VBoxMou.def ^
    -out:VBoxMou\%OBJDIR%\VBoxMou.vxd -map:VBoxMou\%OBJDIR%\VBoxMou.map VBoxMou\%OBJDIR%\VBoxMou.obj
if errorlevel 1 goto failed

rem VBoxSF.vxd: C with a small assembler part, compiled and linked the same way.
set SFCFLAGS=-nologo -c -W3 -WX -Zp -Gs -Zl -DIS_32 -I"%DDK98DIR%\inc\win98"
if /i "%BUILDTYPE%"=="free" set SFCFLAGS=%SFCFLAGS% -O2
if /i "%BUILDTYPE%"=="checked" set SFCFLAGS=%SFCFLAGS% -Od
for %%f in (VBoxSF Host Net File Find Names) do (
    cl %SFCFLAGS% -FoVBoxSF\%OBJDIR%\%%f.obj VBoxSF\%%f.c
    if errorlevel 1 goto failed_sf
)
"%DDK98DIR%\bin\win98\ml.exe" -nologo -coff -DBLD_COFF -DIS_32 -DMASM6 -DDEBLEVEL=0 -W2 -c -Cx -Zm ^
    -I"%DDK98DIR%\inc\win98" -FoVBoxSF\%OBJDIR%\VBoxSFA.obj VBoxSF\VBoxSFA.asm
if errorlevel 1 goto failed_sf
"%DDK98DIR%\bin\link.exe" -nologo -vxd -nod -ignore:4078 -ignore:4039 -def:VBoxSF\VBoxSF.def ^
    -out:VBoxSF\%OBJDIR%\VBoxSF.vxd -map:VBoxSF\%OBJDIR%\VBoxSF.map VBoxSF\%OBJDIR%\VBoxSFA.obj ^
    VBoxSF\%OBJDIR%\VBoxSF.obj VBoxSF\%OBJDIR%\Host.obj VBoxSF\%OBJDIR%\Net.obj VBoxSF\%OBJDIR%\File.obj ^
    VBoxSF\%OBJDIR%\Find.obj VBoxSF\%OBJDIR%\Names.obj
if errorlevel 1 goto failed_sf

if not exist "%NSIS2DIR%\makensis.exe" goto no_nsis
"%NSIS2DIR%\makensis.exe" /V2 /DBUILDTYPE=%BUILDTYPE% installer\VBoxGuest98.nsi
if errorlevel 1 goto failed_nsis

rem The floppy image with the free installer as SETUP.EXE, when Python 3 is there.
if /i not "%BUILDTYPE%"=="free" goto end
"%PYTHON%" --version >nul 2>&1
if errorlevel 1 goto no_python
for /f "tokens=3" %%v in ('findstr /c:"#define VBOXGUEST98_VERSION_STRING" inc\VBoxGuest98.h') do set VBGVERSION=%%~v
"%PYTHON%" installer\mkfloppy.py installer\obj\VBoxGuest98-%VBGVERSION%.img ^
    installer\obj\VBoxGuest98-%VBGVERSION%.exe=SETUP.EXE
if errorlevel 1 goto failed_floppy
goto end

:no_nsis
echo NSIS 2.46 not found in %NSIS2DIR% (set NSIS2DIR); installer not built.
goto end

:no_python
echo Python 3 not found as "%PYTHON%" (set PYTHON); floppy image not built.
goto end

:usage
echo Usage: build.cmd [free^|checked]
exit /b 1

:no_msvc
echo Visual C++ 6.0 not found in %MSVC6DIR% (set MSVC6DIR).
exit /b 1

:no_ddk98
echo Windows 98 DDK not found in %DDK98DIR% (set DDK98DIR).
exit /b 1

:failed
echo VBoxMou.vxd build failed.
exit /b 1

:failed_sf
echo VBoxSF.vxd build failed.
exit /b 1

:failed_nsis
echo Installer build failed.
exit /b 1

:failed_floppy
echo Floppy image build failed.
exit /b 1

:end
endlocal
