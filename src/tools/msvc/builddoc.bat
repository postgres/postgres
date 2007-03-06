@echo off
REM Adjust path for your docbook installation
SET ROOT=c:\prog\pgsql\docbook

SETLOCAL
SET STARTDIR=%CD%
SET OPENJADE=openjade-1.3.1
SET DSSSL=docbook-dsssl-1.79

IF EXIST ..\msvc IF EXIST ..\..\..\src cd ..\..\..
IF NOT EXIST doc\src\sgml\version.sgml goto noversion

IF NOT EXIST %ROOT%\%OPENJADE% SET NF=OpenJade
IF NOT EXIST %ROOT%\docbook SET NF=docbook
IF NOT EXIST %ROOT%\%DSSSL% set NF=docbook-dssl

IF NOT "%NF%" == "" GOTO notfound

IF "%1" == "renamefiles" GOTO renamefiles

cmd /v /c "%0" renamefiles

cd doc\src\sgml

SET SGML_CATALOG_FILES=%ROOT%\%OPENJADE%\dsssl\catalog;%ROOT%\docbook\docbook.cat
perl %ROOT%\%DSSSL%\bin\collateindex.pl -f -g -o bookindex.sgml -N
perl mk_feature_tables.pl YES ..\..\..\src\backend\catalog\sql_feature_packages.txt ..\..\..\src\backend\catalog\sql_features.txt > features-supported.sgml
perl mk_feature_tables.pl NO ..\..\..\src\backend\catalog\sql_feature_packages.txt ..\..\..\src\backend\catalog\sql_features.txt > features-unsupported.sgml

%ROOT%\%OPENJADE%\bin\openjade -V draft-mode -wall -wno-unused-param -wno-empty -D . -c %ROOT%\%DSSSL%\catalog -d stylesheet.dsl -i output-html -t sgml postgres.sgml
perl %ROOT%\%DSSSL%\bin\collateindex.pl -f -g -i 'bookindex' -o bookindex.sgml HTML.index
%ROOT%\%OPENJADE%\bin\openjade -V draft-mode -wall -wno-unused-param -wno-empty -D . -c %ROOT%\%DSSSL%\catalog -d stylesheet.dsl -i output-html -t sgml postgres.sgml

cd %STARTDIR%
echo Docs build complete.
exit /b


:renamefiles
REM Rename ISO entity files
CD %ROOT%\docbook
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
