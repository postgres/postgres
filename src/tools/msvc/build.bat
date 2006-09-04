@echo off
SET STARTDIR=%CD%

perl mkvcbuild.pl
if errorlevel 1 goto :eof

if exist ..\vcbuild if exist ..\src cd ..

if "%1" == "" msbuild pgsql.sln
if not "%1" == "" vcbuild %1.vcproj

cd %STARTDIR%
