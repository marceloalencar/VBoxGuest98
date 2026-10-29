@echo off
rem Builds all components with Visual C++ 6.0 and the Windows 98 DDK, then the
rem installer with NSIS 2.46 when it is available.
rem
rem Usage: build.cmd [free|checked]
rem
rem Override the tool locations with the MSVC6DIR, DDK98DIR and NSIS2DIR
rem environment variables (defaults: C:\MSVC600, C:\98DDK and C:\NSIS246).

setlocal
if "%MSVC6DIR%"=="" set MSVC6DIR=C:\MSVC600
if "%DDK98DIR%"=="" set DDK98DIR=C:\98DDK
if "%NSIS2DIR%"=="" set NSIS2DIR=C:\NSIS246
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
for %%d in (VBoxGst VBoxTray VBoxNP VBoxMou VBoxSF) do if not exist %%d\%OBJDIR% mkdir %%d\%OBJDIR%
build -cZ

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
goto end

:no_nsis
echo NSIS 2.46 not found in %NSIS2DIR% (set NSIS2DIR); installer not built.
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

:end
endlocal
