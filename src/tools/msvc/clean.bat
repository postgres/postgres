@echo off
REM src/tools/msvc/clean.bat

set DIST=0
if "%1"=="dist" set DIST=1

set D=%CD%
if exist ..\msvc if exist ..\..\..\src cd ..\..\..

if exist debug rd /s /q debug
if exist release rd /s /q release
for %%f in (*.vcproj) do del %%f
for %%f in (*.vcxproj) do del %%f
for %%f in (*.vcxproj.user) do del %%f
if exist pgsql.sln del /q pgsql.sln
if exist pgsql.sln.cache del /q pgsql.sln.cache
if exist pgsql.sdf del /q pgsql.sdf
if exist pgsql.suo del /q /a:H pgsql.suo
del /s /q src\bin\win32ver.rc 2> NUL
del /s /q src\interfaces\win32ver.rc 2> NUL
if exist src\backend\win32ver.rc del /q src\backend\win32ver.rc
if exist src\backend\replication\libpqwalreceiver\win32ver.rc del /q src\backend\replication\libpqwalreceiver\win32ver.rc
if exist src\backend\snowball\win32ver.rc del /q src\backend\snowball\win32ver.rc
if exist src\interfaces\ecpg\test\win32ver.rc del /q src\interfaces\ecpg\test\win32ver.rc
if exist src\pl\plperl\win32ver.rc del /q src\pl\plperl\win32ver.rc
if exist src\pl\plpgsql\src\win32ver.rc del /q src\pl\plpgsql\src\win32ver.rc
if exist src\pl\plpython\win32ver.rc del /q src\pl\plpython\win32ver.rc
if exist src\pl\tcl\win32ver.rc del /q src\pl\tcl\win32ver.rc
if exist src\test\isolation\win32ver.rc del /q src\test\isolation\win32ver.rc
if exist src\test\regress\win32ver.rc del /q src\test\regress\win32ver.rc
if exist src\timezone\win32ver.rc del /q src\timezone\win32ver.rc

for /d %%f in (src\interfaces\ecpg\*) do if exist %%f\win32ver.rc del /q %%f\win32ver.rc
for /d %%f in (contrib\*) do if exist %%f\win32ver.rc del /q %%f\win32ver.rc
for /d %%f in (src\backend\utils\mb\conversion_procs\*) do if exist %%f\win32ver.rc del /q %%f\win32ver.rc

REM Delete files created with GenerateFiles() in Solution.pm
if exist src\include\pg_config.h del /q src\include\pg_config.h
if exist src\include\pg_config_ext.h del /q src\include\pg_config_ext.h
if exist src\include\pg_config_os.h del /q src\include\pg_config_os.h
if exist src\include\dynloader.h del /q src\include\dynloader.h
if %DIST%==1 if exist src\backend\parser\gram.h del /q src\backend\parser\gram.h
if exist src\include\utils\errcodes.h del /q src\include\utils\errcodes.h
if exist src\include\utils\fmgroids.h del /q src\include\utils\fmgroids.h
if exist src\include\utils\probes.h del /q src\include\utils\probes.h

if %DIST%==1 if exist src\backend\utils\fmgroids.h del /q src\backend\utils\fmgroids.h
if %DIST%==1 if exist src\backend\utils\fmgrtab.c del /q src\backend\utils\fmgrtab.c
if %DIST%==1 if exist src\backend\catalog\postgres.bki del /q src\backend\catalog\postgres.bki
if %DIST%==1 if exist src\backend\catalog\postgres.description del /q src\backend\catalog\postgres.description
if %DIST%==1 if exist src\backend\catalog\postgres.shdescription del /q src\backend\catalog\postgres.shdescription
if %DIST%==1 if exist src\backend\catalog\schemapg.h del /q src\backend\catalog\schemapg.h
if %DIST%==1 if exist src\backend\parser\scan.c del /q src\backend\parser\scan.c
if %DIST%==1 if exist src\backend\parser\gram.c del /q src\backend\parser\gram.c
if %DIST%==1 if exist src\backend\bootstrap\bootscanner.c del /q src\backend\bootstrap\bootscanner.c
if %DIST%==1 if exist src\backend\bootstrap\bootparse.c del /q src\backend\bootstrap\bootparse.c
if %DIST%==1 if exist src\backend\utils\misc\guc-file.c del /q src\backend\utils\misc\guc-file.c


if exist src\bin\psql\sql_help.h del /q src\bin\psql\sql_help.h

if exist src\interfaces\libpq\libpq.rc del /q src\interfaces\libpq\libpq.rc
if exist src\interfaces\libpq\libpqdll.def del /q src\interfaces\libpq\libpqdll.def
if exist src\interfaces\ecpg\compatlib\compatlib.def del /q src\interfaces\ecpg\compatlib\compatlib.def
if exist src\interfaces\ecpg\ecpglib\ecpglib.def del /q src\interfaces\ecpg\ecpglib\ecpglib.def
if exist src\interfaces\ecpg\include\ecpg_config.h del /q src\interfaces\ecpg\include\ecpg_config.h
if exist src\interfaces\ecpg\pgtypeslib\pgtypeslib.def del /q src\interfaces\ecpg\pgtypeslib\pgtypeslib.def
if %DIST%==1 if exist src\interfaces\ecpg\preproc\pgc.c del /q src\interfaces\ecpg\preproc\pgc.c
if %DIST%==1 if exist src\interfaces\ecpg\preproc\preproc.c del /q src\interfaces\ecpg\preproc\preproc.c
if %DIST%==1 if exist src\interfaces\ecpg\preproc\preproc.h del /q src\interfaces\ecpg\preproc\preproc.h

if exist src\port\pg_config_paths.h del /q src\port\pg_config_paths.h

if exist src\pl\plperl\spi.c del /q src\pl\plperl\spi.c
if %DIST%==1 if exist src\pl\plpgsql\src\pl_gram.c del /q src\pl\plpgsql\src\pl_gram.c
if %DIST%==1 if exist src\pl\plpgsql\src\pl_gram.h del /q src\pl\plpgsql\src\pl_gram.h

if %DIST%==1 if exist src\bin\psql\psqlscan.c del /q src\bin\psql\psqlscan.c
if %DIST%==1 if exist src\bin\psql\psqlscanslash.c del /q src\bin\psql\psqlscanslash.c

if %DIST%==1 if exist contrib\cube\cubescan.c del /q contrib\cube\cubescan.c
if %DIST%==1 if exist contrib\cube\cubeparse.c del /q contrib\cube\cubeparse.c
if %DIST%==1 if exist contrib\seg\segscan.c del /q contrib\seg\segscan.c
if %DIST%==1 if exist contrib\seg\segparse.c del /q contrib\seg\segparse.c

if exist src\test\regress\tmp_check rd /s /q src\test\regress\tmp_check
if exist contrib\spi\refint.dll del /q contrib\spi\refint.dll
if exist contrib\spi\autoinc.dll del /q contrib\spi\autoinc.dll
if exist src\test\regress\regress.dll del /q src\test\regress\regress.dll
if exist src\test\regress\refint.dll del /q src\test\regress\refint.dll
if exist src\test\regress\autoinc.dll del /q src\test\regress\autoinc.dll

if exist src\bin\initdb\tmp_check rd /s /q src\bin\initdb\tmp_check
if exist src\bin\pg_basebackup\tmp_check rd /s /q src\bin\pg_basebackup\tmp_check
if exist src\bin\pg_config\tmp_check rd /s /q src\bin\pg_config\tmp_check
if exist src\bin\pg_controldata\tmp_check rd /s /q src\bin\pg_controldata\tmp_check
if exist src\bin\pg_ctl\tmp_check rd /s /q src\bin\pg_ctl\tmp_check
if exist src\bin\pg_rewind\tmp_check rd /s /q src\bin\pg_rewind\tmp_check
if exist src\bin\pgbench\tmp_check rd /s /q src\bin\pgbench\tmp_check
if exist src\bin\scripts\tmp_check rd /s /q src\bin\scripts\tmp_check
if exist src\test\recovery\tmp_check rd /s /q src\test\recovery\tmp_check

REM Clean up datafiles built with contrib
REM cd contrib
REM for /r %%f in (*.sql) do if exist %%f.in del %%f

cd %D%

REM Clean up ecpg regression test files
msbuild /NoLogo ecpg_regression.proj /t:clean /v:q

goto :eof
