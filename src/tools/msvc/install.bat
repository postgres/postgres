@echo off
REM $PostgreSQL: pgsql/src/tools/msvc/install.bat,v 1.1 2007/05/01 20:11:14 mha Exp $

if NOT "%1"=="" GOTO RUN_INSTALL

echo Invalid command line options.
echo Usage: "install.bat <path>"
echo.
exit /b 1

:RUN_INSTALL

SETLOCAL
if exist buildenv.bat call buildenv.bat

perl install.pl "%1"

exit /b %ERRORLEVEL%
