@echo off

REM src/tools/msvc/pgflex.bat
REM all the logic for this now belongs in pgflex.pl. This file really
REM only exists so you don't have to type "perl src/tools/msvc/pgflex.pl"
REM Resist any temptation to add any logic here.
@perl %~dp0/pgflex.pl %*
