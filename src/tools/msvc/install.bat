@echo off
REM $PostgreSQL: pgsql/src/tools/msvc/install.bat,v 1.2 2007/06/26 11:43:56 mha Exp $

if NOT "%1"=="" GOTO RUN_INSTALL

echo Invalid command line options.
echo Usage: "install.bat <path>"
echo.
REM exit fix for pre-2003 shell especially if used on buildfarm
if "%XP_EXIT_FIX%" == "yes" exit 1
exit /b 1

:RUN_INSTALL

SETLOCAL
if exist buildenv.bat call buildenv.bat

perl install.pl "%1"

REM exit fix for pre-2003 shell especially if used on buildfarm
if "%XP_EXIT_FIX%" == "yes" exit %ERRORLEVEL%
exit /b %ERRORLEVEL%
