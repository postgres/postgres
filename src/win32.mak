# src/win32.mak

# Top-file makefile for building Win32 libpq with Visual C++ 7.1.
# (see src/tools/msvc for tools to build with Visual C++ 2005 and newer)

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE
NULL=nul
!ENDIF

ALL:
   cd include
   if not exist pg_config.h copy pg_config.h.win32 pg_config.h
   if not exist pg_config_ext.h copy pg_config_ext.h.win32 pg_config_ext.h
   if not exist pg_config_os.h copy port\win32.h pg_config_os.h
   cd ..
   cd interfaces\libpq
   nmake /f win32.mak $(MAKEMACRO)
   cd ..\..
   echo All Win32 parts have been built!

CLEAN:
   cd interfaces\libpq
   nmake /f win32.mak CLEAN
   cd ..\..
   echo All Win32 parts have been cleaned!

DISTCLEAN: CLEAN
   cd include
   del pg_config.h pg_config_ext.h pg_config_os.h
   cd ..
