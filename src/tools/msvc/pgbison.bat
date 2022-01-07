@echo off

REM src/tools/msvc/pgbison.bat
REM all the logic for this now belongs in pgbison.pl. This file really
REM only exists so you don't have to type "perl src/tools/msvc/pgbison.pl"
REM Resist any temptation to add any logic here.
@perl %~dp0/pgbison.pl %*
