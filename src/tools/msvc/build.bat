@echo off
REM $PostgreSQL: pgsql/src/tools/msvc/build.bat,v 1.10 2007/09/27 21:13:11 adunstan Exp $
REM all the logic for this now belongs in build.pl. This file really
REM only exists so you don't have to type "perl build.pl"
REM Resist any temptation to add any logic here.
@perl build.pl %*
