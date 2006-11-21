# Makefile for Microsoft Visual C++ 5.0 (or compat)

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

CPP=cl.exe

!IFDEF DEBUG
OPT=/Od /Zi /MDd
LOPT=/DEBUG
DEBUGDEF=/D _DEBUG
OUTDIR=.\Debug
INTDIR=.\Debug
!ELSE
OPT=/O2 /MD
LOPT=
DEBUGDEF=/D NDEBUG
OUTDIR=.\Release
INTDIR=.\Release
!ENDIF

ALL : "..\..\port\pg_config_paths.h" "$(OUTDIR)\pg_config.exe"

CLEAN :
	-@erase "$(INTDIR)\pg_config.obj"
	-@erase "$(INTDIR)\pgstrcasecmp.obj"
	-@erase "$(OUTDIR)\path.obj"
	-@erase "$(OUTDIR)\strlcpy.obj"
	-@erase "$(INTDIR)\exec.obj"
	-@erase "$(INTDIR)\snprintf.obj"
	-@erase "$(OUTDIR)\pg_config.exe"
	-@erase "$(INTDIR)\..\..\port\pg_config_paths.h"

"..\..\port\pg_config_paths.h": win32.mak
	echo #define PGBINDIR "" >$@
	echo #define PGSHAREDIR "" >>$@
	echo #define SYSCONFDIR "" >>$@
	echo #define INCLUDEDIR "" >>$@
	echo #define PKGINCLUDEDIR "" >>$@
	echo #define INCLUDEDIRSERVER "" >>$@
	echo #define LIBDIR "" >>$@
	echo #define PKGLIBDIR "" >>$@
	echo #define LOCALEDIR "" >>$@
	echo #define DOCDIR "" >>$@
	echo #define MANDIR "" >>$@

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo $(OPT) /W3 /EHsc /D "WIN32" $(DEBUGDEF) /D "_CONSOLE" /D\
 "_MBCS" /Fp"$(INTDIR)\pg_config.pch" /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c \
 /I ..\..\include /I ..\..\interfaces\libpq /I ..\..\include\port\win32 \
 /I ..\..\include\port\win32_msvc /D "FRONTEND" \
 /D "_CRT_SECURE_NO_DEPRECATE"

CPP_OBJS=$(INTDIR)/
CPP_SBRS=.

LINK32=link.exe
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib shfolder.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib wsock32.lib /nologo /subsystem:console /incremental:no\
 /pdb:"$(OUTDIR)\pg_config.pdb" /machine:I386 $(LOPT) /out:"$(OUTDIR)\pg_config.exe" 
LINK32_OBJS= \
	"$(INTDIR)\pg_config.obj" \
	"$(INTDIR)\pgstrcasecmp.obj" \
	"$(OUTDIR)\path.obj" \
	"$(OUTDIR)\strlcpy.obj" \
	"$(INTDIR)\exec.obj" \
	"$(INTDIR)\snprintf.obj" \
!IFDEF DEBUG
	"..\..\interfaces\libpq\Debug\libpqddll.lib"
!ELSE
	"..\..\interfaces\libpq\Release\libpqdll.lib"
!ENDIF

"$(OUTDIR)\pg_config.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

"$(OUTDIR)\path.obj" : "$(OUTDIR)" ..\..\port\path.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\path.c
<<

"$(OUTDIR)\strlcpy.obj" : "$(OUTDIR)" ..\..\port\strlcpy.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\strlcpy.c
<<

"$(INTDIR)\pgstrcasecmp.obj" : ..\..\port\pgstrcasecmp.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\pgstrcasecmp.c
<<

"$(INTDIR)\exec.obj" : ..\..\port\exec.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\exec.c
<<

"$(INTDIR)\snprintf.obj" : ..\..\port\snprintf.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\snprintf.c
<<

.c{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cpp{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

