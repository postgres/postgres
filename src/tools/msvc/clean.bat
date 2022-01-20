@echo off
REM src/tools/msvc/clean.bat

set DIST=0
if "%1"=="dist" set DIST=1

setlocal

cd "%~dp0\..\..\.."

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
if exist src\backend\replication\pgoutput\win32ver.rc del /q src\backend\replication\pgoutput\win32ver.rc
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
for /d %%f in (src\test\modules\*) do if exist %%f\win32ver.rc del /q %%f\win32ver.rc

REM Delete files created with GenerateFiles() in Solution.pm
if exist src\include\pg_config.h del /q src\include\pg_config.h
if exist src\include\pg_config_ext.h del /q src\include\pg_config_ext.h
if exist src\include\pg_config_os.h del /q src\include\pg_config_os.h
if %DIST%==1 if exist src\backend\parser\gram.h del /q src\backend\parser\gram.h
if exist src\include\utils\errcodes.h del /q src\include\utils\errcodes.h
if exist src\include\utils\fmgroids.h del /q src\include\utils\fmgroids.h
if exist src\include\utils\fmgrprotos.h del /q src\include\utils\fmgrprotos.h
if exist src\include\storage\lwlocknames.h del /q src\include\storage\lwlocknames.h
if exist src\include\utils\probes.h del /q src\include\utils\probes.h
if exist src\include\catalog\schemapg.h del /q src\include\catalog\schemapg.h
if exist src\include\catalog\pg_*_d.h del /q src\include\catalog\pg_*_d.h
if exist src\include\catalog\header-stamp del /q src\include\catalog\header-stamp
if exist doc\src\sgml\version.sgml del /q doc\src\sgml\version.sgml

if %DIST%==1 if exist src\backend\utils\fmgroids.h del /q src\backend\utils\fmgroids.h
if %DIST%==1 if exist src\backend\utils\fmgrprotos.h del /q src\backend\utils\fmgrprotos.h
if %DIST%==1 if exist src\backend\utils\fmgrtab.c del /q src\backend\utils\fmgrtab.c
if %DIST%==1 if exist src\backend\utils\fmgr-stamp del /q src\backend\utils\fmgr-stamp
if %DIST%==1 if exist src\backend\utils\errcodes.h del /q src\backend\utils\errcodes.h
if %DIST%==1 if exist src\backend\storage\lmgr\lwlocknames.c del /q src\backend\storage\lmgr\lwlocknames.c
if %DIST%==1 if exist src\backend\storage\lmgr\lwlocknames.h del /q src\backend\storage\lmgr\lwlocknames.h
if %DIST%==1 if exist src\pl\plpython\spiexceptions.h del /q src\pl\plpython\spiexceptions.h
if %DIST%==1 if exist src\pl\plpgsql\src\plerrcodes.h del /q src\pl\plpgsql\src\plerrcodes.h
if %DIST%==1 if exist src\pl\tcl\pltclerrcodes.h del /q src\pl\tcl\pltclerrcodes.h
if %DIST%==1 if exist src\backend\utils\sort\qsort_tuple.c del /q src\backend\utils\sort\qsort_tuple.c
if %DIST%==1 if exist src\bin\psql\sql_help.c del /q src\bin\psql\sql_help.c
if %DIST%==1 if exist src\bin\psql\sql_help.h del /q src\bin\psql\sql_help.h
if %DIST%==1 if exist src\common\kwlist_d.h del /q src\common\kwlist_d.h
if %DIST%==1 if exist src\pl\plpgsql\src\pl_reserved_kwlist_d.h del /q src\pl\plpgsql\src\pl_reserved_kwlist_d.h
if %DIST%==1 if exist src\pl\plpgsql\src\pl_unreserved_kwlist_d.h del /q src\pl\plpgsql\src\pl_unreserved_kwlist_d.h
if %DIST%==1 if exist src\interfaces\ecpg\preproc\c_kwlist_d.h del /q src\interfaces\ecpg\preproc\c_kwlist_d.h
if %DIST%==1 if exist src\interfaces\ecpg\preproc\ecpg_kwlist_d.h del /q src\interfaces\ecpg\preproc\ecpg_kwlist_d.h
if %DIST%==1 if exist src\interfaces\ecpg\preproc\preproc.y del /q src\interfaces\ecpg\preproc\preproc.y
if %DIST%==1 if exist src\backend\catalog\postgres.bki del /q src\backend\catalog\postgres.bki
if %DIST%==1 if exist src\backend\catalog\schemapg.h del /q src\backend\catalog\schemapg.h
if %DIST%==1 if exist src\backend\catalog\pg_*_d.h del /q src\backend\catalog\pg_*_d.h
if %DIST%==1 if exist src\backend\catalog\bki-stamp del /q src\backend\catalog\bki-stamp
if %DIST%==1 if exist src\backend\parser\scan.c del /q src\backend\parser\scan.c
if %DIST%==1 if exist src\backend\parser\gram.c del /q src\backend\parser\gram.c
if %DIST%==1 if exist src\backend\bootstrap\bootscanner.c del /q src\backend\bootstrap\bootscanner.c
if %DIST%==1 if exist src\backend\bootstrap\bootparse.c del /q src\backend\bootstrap\bootparse.c
if %DIST%==1 if exist src\backend\utils\adt\jsonpath_gram.c del /q src\backend\utils\adt\jsonpath_gram.c
if %DIST%==1 if exist src\backend\utils\adt\jsonpath_scan.c del /q src\backend\utils\adt\jsonpath_scan.c
if %DIST%==1 if exist src\backend\utils\misc\guc-file.c del /q src\backend\utils\misc\guc-file.c
if %DIST%==1 if exist src\backend\replication\repl_scanner.c del /q src\backend\replication\repl_scanner.c
if %DIST%==1 if exist src\backend\replication\repl_gram.c del /q src\backend\replication\repl_gram.c
if %DIST%==1 if exist src\backend\replication\syncrep_scanner.c del /q src\backend\replication\syncrep_scanner.c
if %DIST%==1 if exist src\backend\replication\syncrep_gram.c del /q src\backend\replication\syncrep_gram.c


if exist src\interfaces\libpq\libpqdll.def del /q src\interfaces\libpq\libpqdll.def
if exist src\interfaces\ecpg\compatlib\compatlib.def del /q src\interfaces\ecpg\compatlib\compatlib.def
if exist src\interfaces\ecpg\ecpglib\ecpglib.def del /q src\interfaces\ecpg\ecpglib\ecpglib.def
if exist src\interfaces\ecpg\include\ecpg_config.h del /q src\interfaces\ecpg\include\ecpg_config.h
if exist src\interfaces\ecpg\pgtypeslib\pgtypeslib.def del /q src\interfaces\ecpg\pgtypeslib\pgtypeslib.def
if %DIST%==1 if exist src\interfaces\ecpg\preproc\pgc.c del /q src\interfaces\ecpg\preproc\pgc.c
if %DIST%==1 if exist src\interfaces\ecpg\preproc\preproc.c del /q src\interfaces\ecpg\preproc\preproc.c
if %DIST%==1 if exist src\interfaces\ecpg\preproc\preproc.h del /q src\interfaces\ecpg\preproc\preproc.h

if exist src\port\pg_config_paths.h del /q src\port\pg_config_paths.h

if exist src\pl\plperl\SPI.c del /q src\pl\plperl\SPI.c
if exist src\pl\plperl\Util.c del /q src\pl\plperl\Util.c
if exist src\pl\plperl\perlchunks.h del /q src\pl\plperl\perlchunks.h
if exist src\pl\plperl\plperl_opmask.h del /q src\pl\plperl\plperl_opmask.h
if %DIST%==1 if exist src\pl\plpgsql\src\pl_gram.c del /q src\pl\plpgsql\src\pl_gram.c
if %DIST%==1 if exist src\pl\plpgsql\src\pl_gram.h del /q src\pl\plpgsql\src\pl_gram.h

if %DIST%==1 if exist src\fe_utils\psqlscan.c del /q src\fe_utils\psqlscan.c
if %DIST%==1 if exist src\bin\psql\psqlscanslash.c del /q src\bin\psql\psqlscanslash.c
if %DIST%==1 if exist src\bin\pgbench\exprscan.c del /q src\bin\pgbench\exprscan.c
if %DIST%==1 if exist src\bin\pgbench\exprparse.c del /q src\bin\pgbench\exprparse.c

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
if %DIST%==1 if exist src\test\isolation\specscanner.c del /q src\test\isolation\specscanner.c
if %DIST%==1 if exist src\test\isolation\specparse.c del /q src\test\isolation\specparse.c

for /d %%f in (contrib\* src\bin\* src\test\* src\test\modules\*
  ) do if exist %%f\tmp_check rd /s /q %%f\tmp_check

REM Clean up datafiles built with contrib
REM cd contrib
REM for /r %%f in (*.sql) do if exist %%f.in del %%f

cd "%~dp0"

REM Clean up ecpg regression test files
msbuild ecpg_regression.proj /NoLogo /v:q %MSBFLAGS% /t:clean

goto :eof
