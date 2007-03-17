@echo off
SETLOCAL
SET STARTDIR=%CD%
if exist ..\..\..\src\tools\msvc\vcregress.bat cd ..\..\..
if exist src\tools\msvc\buildenv.bat call src\tools\msvc\buildenv.bat

set what=
if /I "%1"=="check" SET what=CHECK
if /I "%1"=="installcheck" SET what=INSTALLCHECK
if "%what%"=="" goto usage

SET CONFIG=Debug
if exist release\postgres\postgres.exe SET CONFIG=Release

copy %CONFIG%\refint\refint.dll contrib\spi\
copy %CONFIG%\autoinc\autoinc.dll contrib\spi\
copy %CONFIG%\regress\regress.dll src\test\regress\

SET PATH=..\..\..\%CONFIG%\libpq;%PATH%

SET TOPDIR=%CD%
cd src\test\regress
SET SCHEDULE=parallel
SET TEMPPORT=54321
IF NOT "%2"=="" SET SCHEDULE=%2

SET PERL5LIB=..\..\tools\msvc

if "%what%"=="INSTALLCHECK" ..\..\..\%CONFIG%\pg_regress\pg_regress --psqldir=..\..\..\%CONFIG%\psql --schedule=%SCHEDULE%_schedule --multibyte=SQL_ASCII --load-language=plpgsql --no-locale
if "%what%"=="CHECK" ..\..\..\%CONFIG%\pg_regress\pg_regress --psqldir=..\..\..\%CONFIG%\psql --schedule=%SCHEDULE%_schedule --multibyte=SQL_ASCII --load-language=plpgsql --no-locale --temp-install=./tmp_check --top-builddir=%TOPDIR% --temp-port=%TEMPPORT%

cd %STARTDIR%
goto :eof

:usage
echo "Usage: vcregress <check|installcheck> [schedule]"
goto :eof
