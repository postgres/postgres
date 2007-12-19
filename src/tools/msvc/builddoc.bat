@echo off
REM Adjust path for your docbook installation in buildenv.pl

REM $PostgreSQL: pgsql/src/tools/msvc/builddoc.bat,v 1.6 2007/12/19 12:29:36 mha Exp $

SETLOCAL
SET STARTDIR=%CD%
SET OPENJADE=openjade-1.3.1
SET DSSSL=docbook-dsssl-1.79

IF EXIST ..\msvc IF EXIST ..\..\..\src cd ..\..\..
IF NOT EXIST doc\src\sgml\version.sgml goto noversion

IF NOT EXIST src\tools\msvc\buildenv.pl goto nobuildenv
perl -e "require 'src/tools/msvc/buildenv.pl'; while(($k,$v) = each %ENV) { print qq[\@SET $k=$v\n]; }" > bldenv.bat
CALL bldenv.bat
del bldenv.bat
:nobuildenv 

IF NOT EXIST %DOCROOT%\%OPENJADE% SET NF=OpenJade
IF NOT EXIST %DOCROOT%\docbook SET NF=docbook
IF NOT EXIST %DOCROOT%\%DSSSL% set NF=docbook-dssl

IF NOT "%NF%" == "" GOTO notfound

IF "%1" == "renamefiles" GOTO renamefiles

cmd /v /c src\tools\msvc\builddoc renamefiles
cd doc\src\sgml

SET SGML_CATALOG_FILES=%DOCROOT%\%OPENJADE%\dsssl\catalog;%DOCROOT%\docbook\docbook.cat
perl %DOCROOT%\%DSSSL%\bin\collateindex.pl -f -g -o bookindex.sgml -N
perl mk_feature_tables.pl YES ..\..\..\src\backend\catalog\sql_feature_packages.txt ..\..\..\src\backend\catalog\sql_features.txt > features-supported.sgml
perl mk_feature_tables.pl NO ..\..\..\src\backend\catalog\sql_feature_packages.txt ..\..\..\src\backend\catalog\sql_features.txt > features-unsupported.sgml

echo Running first build...
%DOCROOT%\%OPENJADE%\bin\openjade -V draft-mode -wall -wno-unused-param -wno-empty -D . -c %DOCROOT%\%DSSSL%\catalog -d stylesheet.dsl -i output-html -t sgml postgres.sgml 2>&1 | findstr /V "DTDDECL catalog entries are not supported"
echo Running collateindex...
perl %DOCROOT%\%DSSSL%\bin\collateindex.pl -f -g -i bookindex -o bookindex.sgml HTML.index
echo Running second build...
%DOCROOT%\%OPENJADE%\bin\openjade -V draft-mode -wall -wno-unused-param -wno-empty -D . -c %DOCROOT%\%DSSSL%\catalog -d stylesheet.dsl -i output-html -t sgml postgres.sgml 2>&1 | findstr /V "DTDDECL catalog entries are not supported"

cd %STARTDIR%
echo Docs build complete.
exit /b


:renamefiles
REM Rename ISO entity files
CD %DOCROOT%\docbook
FOR %%f in (ISO*) do (
   set foo=%%f
   IF NOT "!foo:~-4!" == ".gml" ren !foo! !foo:~0,3!-!foo:~3!.gml
)
exit /b

:notfound
echo Could not find directory for %NF%.
cd %STARTDIR%
goto :eof

:noversion
echo Could not find version.sgml. Please run mkvcbuild.pl first!
cd %STARTDIR%
goto :eof
