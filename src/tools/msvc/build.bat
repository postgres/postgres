@echo off
SET STARTDIR=%CD%

perl mkvcbuild.pl
if errorlevel 1 goto :eof

if exist ..\msvc if exist ..\..\..\src cd ..\..\..
SET CONFIG=
if "%1" == "" set CONFIG=Debug
if "%CONFIG%" == "" if "%1" == "DEBUG" set CONFIG=Debug
if "%CONFIG%" == "" if "%1" == "RELEASE" set CONFIG=Release
if not "%CONFIG%" == "" shift
if "%CONFIG%" == "" set CONFIG=Debug

if "%1" == "" msbuild pgsql.sln /p:Configuration=%CONFIG%
if not "%1" == "" vcbuild %1.vcproj %CONFIG%

cd %STARTDIR%
