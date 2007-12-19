@echo off
REM $PostgreSQL: pgsql/src/tools/msvc/clean.bat,v 1.9 2007/12/19 12:31:35 mha Exp $

set D=%CD%
if exist ..\msvc if exist ..\..\..\src cd ..\..\..

if exist debug rd /s /q debug
if exist release rd /s /q release
call :del *.vcproj
call :del pgsql.sln
del /s /q src\bin\win32ver.rc 2> NUL
del /s /q src\interfaces\win32ver.rc 2> NUL
call :del src\backend\win32ver.rc


REM Delete files created with GenerateFiles() in Solution.pm
call :del src\include\pg_config.h
call :del src\include\pg_config_os.h
call :del src\backend\parser\parse.h
call :del src\include\utils\fmgroids.h

call :del src\backend\utils\fmgrtab.c
call :del src\backend\catalog\postgres.bki
call :del src\backend\catalog\postgres.description
call :del src\backend\catalog\postgres.shdescription
call :del src\backend\parser\gram.c
call :del src\backend\bootstrap\bootparse.c
call :del src\backend\bootstrap\bootstrap_tokens.h

call :del src\bin\psql\sql_help.h

call :del src\interfaces\libpq\libpq.rc
call :del src\interfaces\libpq\libpqdll.def
call :del src\interfaces\ecpg\compatlib\compatlib.def
call :del src\interfaces\ecpg\ecpglib\ecpglib.def
call :del src\interfaces\ecpg\include\ecpg_config.h
call :del src\interfaces\ecpg\pgtypeslib\pgtypeslib.def
call :del src\interfaces\ecpg\preproc\preproc.c
call :del src\interfaces\ecpg\preproc\preproc.h

call :del src\port\pg_config_paths.h

call :del src\pl\plperl\spi.c
call :del src\pl\plpgsql\src\pl_gram.c
call :del src\pl\plpgsql\src\pl.tab.h

call :del contrib\cube\cubeparse.c
call :del contrib\cube\cubeparse.h
call :del contrib\seg\segparse.c
call :del contrib\seg\segparse.h

if exist src\test\regress\tmp_check rd /s /q src\test\regress\tmp_check
call :del contrib\spi\refint.dll
call :del contrib\spi\autoinc.dll
call :del src\test\regress\regress.dll

REM Clean up datafiles built with contrib
cd contrib
for /r %%f in (*.sql) do if exist %%f.in del %%f

cd %D%

REM Clean up ecpg regression test files
msbuild /NoLogo ecpg_regression.proj /t:clean /v:q

goto :eof


:del
if exist %1 del /q %1
goto :eof
