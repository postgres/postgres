@echo off
REM $PostgreSQL: pgsql/src/tools/msvc/vcregress.bat,v 1.5 2007/03/21 16:21:40 mha Exp $

SETLOCAL
SET STARTDIR=%CD%
if exist ..\..\..\src\tools\msvc\vcregress.bat cd ..\..\..
if exist src\tools\msvc\buildenv.bat call src\tools\msvc\buildenv.bat

set what=
if /I "%1"=="check" SET what=CHECK
if /I "%1"=="installcheck" SET what=INSTALLCHECK
if /I "%1"=="plcheck" SET what=PLCHECK
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
if "%what%"=="PLCHECK" call :plcheck
SET E=%ERRORLEVEL%

cd %STARTDIR%
exit /b %E%

:usage
echo "Usage: vcregress <check|installcheck> [schedule]"
goto :eof


REM Check procedural languages
REM Some workarounds due to inconsistently named directories
:plcheck
cd ..\..\PL
FOR /D %%d IN (*) do if exist %%d\sql if exist %%d\expected (
   if exist ..\..\%CONFIG%\%%d call :oneplcheck %%d
   if errorlevel 1 exit /b 1
   if exist ..\..\%CONFIG%\pl%%d call :oneplcheck %%d
   if errorlevel 1 exit /b 1
)
goto :eof

REM Check a single procedural language
:oneplcheck
echo Checking %1
cd %1
SET PL=%1
IF %PL%==plpython SET PL=plpythonu
IF %PL%==tcl SET PL=pltcl

perl ../../tools/msvc/getregress.pl > regress.tmp.bat
call regress.tmp.bat
del regress.tmp.bat
..\..\..\%CONFIG%\pg_regress\pg_regress --psqldir=..\..\..\%CONFIG%\psql --no-locale --load-language=%PL% %TESTS%
set E=%ERRORLEVEL%
cd ..
exit /b %E%
