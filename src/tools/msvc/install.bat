@echo off
REM src/tools/msvc/install.bat
REM all the logic for this now belongs in install.pl. This file really
REM only exists so you don't have to type "perl install.pl"
REM Resist any temptation to add any logic here.
@perl %~dp0/install.pl %*
