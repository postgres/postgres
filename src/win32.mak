# Makefile for Microsoft Visual C++ 5.0 (or compat)

# Top-file makefile for Win32 parts of postgresql.

# Note that most parts are not ported to Win32!

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

!IFDEF	MULTIBYTE
MAKEMACRO = "MULTIBYTE=$(MULTIBYTE)"
!ENDIF

ALL: 
   cd interfaces\libpq
   nmake /f win32.mak $(MAKEMACRO)
   cd ..\..\bin\psql
   nmake /f win32.mak $(MAKEMACRO)
   cd ..\..
   echo All Win32 parts have been built!
