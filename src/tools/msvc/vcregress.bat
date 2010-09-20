@echo off
REM src/tools/msvc/vcregress.bat
REM all the logic for this now belongs in vcregress.pl. This file really
REM only exists so you don't have to type "perl vcregress.pl"
REM Resist any temptation to add any logic here.
@perl vcregress.pl %*
