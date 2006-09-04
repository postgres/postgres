@echo off
bison -V > NUL
if errorlevel 1 goto nobison

if "%1" == "src\backend\parser\gram.y" call :generate %1 src\backend\parser\gram.c src\include\parser\parse.h
if "%1" == "src\backend\bootstrap\bootparse.y" call :generate %1 src\backend\bootstrap\bootparse.c src\backend\bootstrap\bootstrap_tokens.h
if "%1" == "src\pl\plpgsql\src\gram.y" call :generate %1 src\pl\plpgsql\src\pl_gram.c src\pl\plpgsql\src\pl.tab.h
if "%1" == "src\interfaces\ecpg\preproc\preproc.y" call :generate %1 src\interfaces\ecpg\preproc\preproc.c src\interfaces\ecpg\preproc\preproc.h

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


:nobison
echo WARNING! Bison install not found, attempting to build without!
exit 0
