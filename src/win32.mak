# $PostgreSQL: pgsql/src/win32.mak,v 1.14 2006/08/08 22:44:05 momjian Exp $

# Makefile for Microsoft Visual C++ 5.0 (or compat)
# Top-file makefile for Win32 parts of postgresql.
# Note that most parts are not ported to Win32!

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

ALL: 
   cd include
   if not exist pg_config.h copy pg_config.h.win32 pg_config.h
   if not exist pg_config_os.h copy port\win32.h pg_config_os.h
   cd ..
   cd interfaces\libpq
   nmake /f win32.mak $(MAKEMACRO)
   cd ..\..\bin\psql
   nmake /f win32.mak $(MAKEMACRO)
   cd ..\..\bin\pg_dump
   nmake /f win32.mak $(MAKEMACRO)
   cd ..\..\bin\pg_config
   nmake /f win32.mak $(MAKEMACRO)
   cd ..\..
   echo All Win32 parts have been built!

CLEAN:
   cd interfaces\libpq
   nmake /f win32.mak CLEAN
   cd ..\..\bin\psql
   nmake /f win32.mak CLEAN
   cd ..\..\bin\pg_dump
   nmake /f win32.mak CLEAN
   cd ..\..\bin\pg_config
   nmake /f win32.mak CLEAN
   cd ..\..
   echo All Win32 parts have been cleaned!

DISTCLEAN: CLEAN
   cd include
   del pg_config.h pg_config_os.h
   cd ..
