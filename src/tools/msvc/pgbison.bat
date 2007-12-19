@echo off
REM $PostgreSQL: pgsql/src/tools/msvc/pgbison.bat,v 1.8 2007/12/19 12:29:36 mha Exp $

IF NOT EXIST src\tools\msvc\buildenv.pl goto nobuildenv
perl -e "require 'src/tools/msvc/buildenv.pl'; while(($k,$v) = each %ENV) { print qq[\@SET $k=$v\n]; }" > bldenv.bat
CALL bldenv.bat
del bldenv.bat
:nobuildenv 

SET BV=
for /F "tokens=4 usebackq" %%f in (`bison -V`) do if "!BV!"=="" SET BV=%%f
if "%BV%"=="" goto novarexp
if %BV% EQU 1.875 goto bisonok
if %BV% GEQ 2.2 goto bisonok
goto nobison
:bisonok

if "%1" == "src\backend\parser\gram.y" call :generate %1 src\backend\parser\gram.c src\backend\parser\parse.h
if "%1" == "src\backend\bootstrap\bootparse.y" call :generate %1 src\backend\bootstrap\bootparse.c src\backend\bootstrap\bootstrap_tokens.h
if "%1" == "src\pl\plpgsql\src\gram.y" call :generate %1 src\pl\plpgsql\src\pl_gram.c src\pl\plpgsql\src\pl.tab.h
if "%1" == "src\interfaces\ecpg\preproc\preproc.y" call :generate %1 src\interfaces\ecpg\preproc\preproc.c src\interfaces\ecpg\preproc\preproc.h
if "%1" == "contrib\cube\cubeparse.y" call :generate %1 contrib\cube\cubeparse.c contrib\cube\cubeparse.h
if "%1" == "contrib\seg\segparse.y" call :generate %1 contrib\seg\segparse.c contrib\seg\segparse.h

echo Unknown bison input: %1
exit 1

:generate
SET fn=%1
SET cf=%2
bison.exe -d %fn% -o %cf%
if errorlevel 1 exit 1
SET hf=%cf:~0,-2%.h
if not "%hf%"=="%3" (
        copy /y %hf% %3
        if errorlevel 1 exit 1
        del %hf%
)
exit 0


:novarexp
echo pgbison must be called with cmd /V:ON /C pgbison to work!
exit 1

:nobison
echo WARNING! Bison install not found, or unsupported Bison version.
echo Attempting to build without.
exit 0
