@echo off
REM src/tools/msvc/install.bat

if NOT "%1"=="" GOTO RUN_INSTALL

echo Invalid command line options.
echo Usage: "install.bat <path>"
echo.
REM exit fix for pre-2003 shell especially if used on buildfarm
if "%XP_EXIT_FIX%" == "yes" exit 1
exit /b 1

:RUN_INSTALL

SETLOCAL

IF NOT EXIST buildenv.pl goto nobuildenv
perl -e "require 'buildenv.pl'; while(($k,$v) = each %%ENV) { print qq[\@SET $k=$v\n]; }" > bldenv.bat
CALL bldenv.bat
del bldenv.bat
:nobuildenv

perl install.pl "%1" %2

REM exit fix for pre-2003 shell especially if used on buildfarm
if "%XP_EXIT_FIX%" == "yes" exit %ERRORLEVEL%
exit /b %ERRORLEVEL%
