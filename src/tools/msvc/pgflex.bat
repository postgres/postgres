@echo off
REM src/tools/msvc/pgflex.bat

REM silence flex bleatings about file path style
SET CYGWIN=nodosfilewarning

IF NOT EXIST src\tools\msvc\buildenv.pl goto nobuildenv
perl -e "require 'src/tools/msvc/buildenv.pl'; while(($k,$v) = each %ENV) { print qq[\@SET $k=$v\n]; }" > bldenv.bat
CALL bldenv.bat
del bldenv.bat
:nobuildenv 

flex -V > NUL
if errorlevel 1 goto noflex

if "%1" == "src\backend\parser\scan.l" call :generate %1 src\backend\parser\scan.c -CF
if "%1" == "src\backend\bootstrap\bootscanner.l" call :generate %1 src\backend\bootstrap\bootscanner.c
if "%1" == "src\backend\utils\misc\guc-file.l" call :generate %1 src\backend\utils\misc\guc-file.c
if "%1" == "src\backend\replication\repl_scanner.l" call :generate %1 src\backend\replication\repl_scanner.c
if "%1" == "src\test\isolation\specscanner.l" call :generate %1 src\test\isolation\specscanner.c
if "%1" == "src\interfaces\ecpg\preproc\pgc.l" call :generate %1 src\interfaces\ecpg\preproc\pgc.c
if "%1" == "src\bin\psql\psqlscan.l" call :generate %1 src\bin\psql\psqlscan.c
if "%1" == "contrib\cube\cubescan.l" call :generate %1 contrib\cube\cubescan.c
if "%1" == "contrib\seg\segscan.l" call :generate %1 contrib\seg\segscan.c

echo Unknown flex input: %1
exit 1

REM For non-reentrant scanners we need to fix up the yywrap macro definition
REM to keep the MS compiler happy.
REM For reentrant scanners (like the core scanner) we do not
REM need to (and must not) change the yywrap definition.
:generate
flex %3 -o%2 %1
if errorlevel 1 exit %errorlevel%
perl -n -e "exit 1 if /^\%%option\s+reentrant/;" %1
if errorlevel 1 exit 0
perl -pi.bak -e "s/yywrap\(n\)/yywrap()/;" %2
if errorlevel 1 exit %errorlevel%
del %2.bak
exit 0

:noflex
echo WARNING! flex install not found, attempting to build without
exit 0
