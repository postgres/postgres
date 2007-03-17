@echo off
REM $PostgreSQL: pgsql/src/tools/msvc/build.bat,v 1.7 2007/03/17 14:01:01 mha Exp $

SETLOCAL
SET STARTDIR=%CD%
if exist src\tools\msvc\buildenv.bat call src\tools\msvc\buildenv.bat
if exist buildenv.bat call buildenv.bat

perl mkvcbuild.pl
if errorlevel 1 goto :eof

if exist ..\msvc if exist ..\..\..\src cd ..\..\..
SET CONFIG=
if "%1" == "" set CONFIG=Debug
if "%CONFIG%" == "" if "%1" == "DEBUG" set CONFIG=Debug
if "%CONFIG%" == "" if "%1" == "RELEASE" set CONFIG=Release
if not "%CONFIG%" == "" shift
if "%CONFIG%" == "" set CONFIG=Debug

if "%1" == "" msbuild pgsql.sln /verbosity:detailed /p:Configuration=%CONFIG%
if not "%1" == "" vcbuild %1.vcproj %CONFIG%
SET E=%ERRORLEVEL%

cd %STARTDIR%

exit /b %E%
