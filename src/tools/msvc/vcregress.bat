@echo off
REM $PostgreSQL: pgsql/src/tools/msvc/vcregress.bat,v 1.14 2007/08/27 12:10:47 mha Exp $

SETLOCAL
SET STARTDIR=%CD%
if exist ..\..\..\src\tools\msvc\vcregress.bat cd ..\..\..
if exist src\tools\msvc\buildenv.bat call src\tools\msvc\buildenv.bat

set what=
if /I "%1"=="check" SET what=CHECK
if /I "%1"=="installcheck" SET what=INSTALLCHECK
if /I "%1"=="plcheck" SET what=PLCHECK
if /I "%1"=="contribcheck" SET what=CONTRIBCHECK
if /I "%1"=="ecpgcheck" SET what=ECPGCHECK
if "%what%"=="" goto usage

SET CONFIG=Debug
if exist release\postgres\postgres.exe SET CONFIG=Release

copy %CONFIG%\refint\refint.dll contrib\spi\
copy %CONFIG%\autoinc\autoinc.dll contrib\spi\
copy %CONFIG%\regress\regress.dll src\test\regress\

SET PATH=..\..\..\%CONFIG%\libpq;..\..\%CONFIG%\libpq;%PATH%

SET TOPDIR=%CD%
cd src\test\regress
SET SCHEDULE=parallel
SET TEMPPORT=54321
IF NOT "%2"=="" SET SCHEDULE=%2

IF "%what%"=="ECPGCHECK" (
   cd "%STARTDIR%"
   msbuild ecpg_regression.proj /p:config=%CONFIG%
   REM exit fix for pre-2003 shell especially if used on buildfarm
   if "%XP_EXIT_FIX%" == "yes" if errorlevel 1 exit 1
   if errorlevel 1 exit /b 1
   cd "%TOPDIR%"
   cd src\interfaces\ecpg\test
   SET SCHEDULE=ecpg
)

SET PERL5LIB=%TOPDIR%\src\tools\msvc

if "%what%"=="INSTALLCHECK" ..\..\..\%CONFIG%\pg_regress\pg_regress --psqldir="..\..\..\%CONFIG%\psql" --schedule=%SCHEDULE%_schedule --multibyte=SQL_ASCII --load-language=plpgsql --no-locale
if "%what%"=="CHECK" ..\..\..\%CONFIG%\pg_regress\pg_regress --psqldir="..\..\..\%CONFIG%\psql" --schedule=%SCHEDULE%_schedule --multibyte=SQL_ASCII --load-language=plpgsql --no-locale --temp-install=./tmp_check --top-builddir="%TOPDIR%" --temp-port=%TEMPPORT%
if "%what%"=="ECPGCHECK" ..\..\..\..\%CONFIG%\pg_regress_ecpg\pg_regress_ecpg --psqldir="..\..\..\%CONFIG%\psql" --dbname=regress1,connectdb --create-role=connectuser,connectdb --schedule=%SCHEDULE%_schedule --multibyte=SQL_ASCII --load-language=plpgsql --no-locale --temp-install=./tmp_check --top-builddir="%TOPDIR%" --temp-port=%TEMPPORT%
if "%what%"=="PLCHECK" call :plcheck
if "%what%"=="CONTRIBCHECK" call :contribcheck
SET E=%ERRORLEVEL%

cd "%STARTDIR%"
REM exit fix for pre-2003 shell especially if used on buildfarm
if "%XP_EXIT_FIX%" == "yes" exit %E%
exit /b %E%

:usage
echo "Usage: vcregress <check|installcheck|plcheck|contribcheck|ecpgcheck> [schedule]"
goto :eof


REM Check procedural languages
REM Some workarounds due to inconsistently named directories
:plcheck
cd ..\..\PL
FOR /D %%d IN (*) do if exist %%d\sql if exist %%d\expected (
   if exist ..\..\%CONFIG%\%%d call :oneplcheck %%d
   REM exit fix for pre-2003 shell especially if used on buildfarm
   if "%XP_EXIT_FIX%" == "yes" if errorlevel 1 exit 1
   if errorlevel 1 exit /b 1
   if exist ..\..\%CONFIG%\pl%%d call :oneplcheck %%d
   if "%XP_EXIT_FIX%" == "yes" if errorlevel 1 exit 1
   if errorlevel 1 exit /b 1
)
goto :eof

REM Check a single procedural language
:oneplcheck
echo ==========================================================================
echo Checking %1
cd %1
SET PL=%1
IF %PL%==plpython SET PL=plpythonu
IF %PL%==tcl SET PL=pltcl

set TESTS=
perl ../../tools/msvc/getregress.pl > regress.tmp.bat
call regress.tmp.bat
del regress.tmp.bat
..\..\..\%CONFIG%\pg_regress\pg_regress --psqldir=..\..\..\%CONFIG%\psql --no-locale --load-language=%PL% %TESTS%
set E=%ERRORLEVEL%
cd ..
REM exit fix for pre-2003 shell especially if used on buildfarm
if "%XP_EXIT_FIX%" == "yes" exit %E%
exit /b %E%


REM Check contrib modules
:contribcheck
cd ..\..\..\contrib
set CONTRIBERROR=0
for /d %%d IN (*) do if exist %%d\sql if exist %%d\expected if exist %%d\Makefile (
   call :onecontribcheck %%d
   if errorlevel 1 set CONTRIBERROR=1
)
REM exit fix for pre-2003 shell especially if used on buildfarm
if "%XP_EXIT_FIX%" == "yes" if %CONTRIBERROR%==1 exit 1
if %CONTRIBERROR%==1 exit /b 1
goto :eof

REM Check a single contrib module
:onecontribcheck
REM Temporarily exclude tsearch2 tests
if %1==tsearch2 goto :eof
cd %1

echo ==========================================================================
echo Checking %1
set TESTS=
perl ../../src/tools/msvc/getregress.pl > regress.tmp.bat
call regress.tmp.bat
del regress.tmp.bat
..\..\%CONFIG%\pg_regress\pg_regress --psqldir=..\..\%CONFIG%\psql --no-locale --dbname=contrib_regression %TESTS%
set E=%ERRORLEVEL%
cd ..
REM exit fix for pre-2003 shell especially if used on buildfarm
if "%XP_EXIT_FIX%" == "yes" exit %E%
exit /b %E%
