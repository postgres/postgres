@echo off
SET BV=
for /F "tokens=4 usebackq" %%f in (`bison -V`) do if "!BV!"=="" SET BV=%%f
if "%BV%"=="" goto novarexp
if %BV% LSS 1.875 goto nobison
if %BV% EQU 2.1 goto nobison

if "%1" == "src\backend\parser\gram.y" call :generate %1 src\backend\parser\gram.c src\include\parser\parse.h
if "%1" == "src\backend\bootstrap\bootparse.y" call :generate %1 src\backend\bootstrap\bootparse.c src\backend\bootstrap\bootstrap_tokens.h
if "%1" == "src\pl\plpgsql\src\gram.y" call :generate %1 src\pl\plpgsql\src\pl_gram.c src\pl\plpgsql\src\pl.tab.h
if "%1" == "src\interfaces\ecpg\preproc\preproc.y" call :generate %1 src\interfaces\ecpg\preproc\preproc.c src\interfaces\ecpg\preproc\preproc.h
if "%1" == "contrib\cube\cubeparse.y" call :generate %1 contrib\cube\cubeparse.c contrib\cube\cubeparse.h
if "%1" == "contrib\seg\segparse.y" call :generate %1 contrib\seg\segparse.c contrib\seg\segparse.h

echo Unknown bison input: %1
exit 1

:generate
SET fn=%1
bison -d %fn%
if errorlevel 1 exit 1
copy /y %fn:~0,-2%.tab.c %2
if errorlevel 1 exit 1
copy /y %fn:~0,-2%.tab.h %3
if errorlevel 1 exit 1
del %fn:~0,-2%.tab.*
exit 0


:novarexp
echo pgbison must be called with cmd /V:ON /C pgbison to work!
exit 1

:nobison
echo WARNING! Bison install not found, or unsupported Bison version.
echo Attempting to build without.
exit 0
