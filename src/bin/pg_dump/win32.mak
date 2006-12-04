# Makefile for Microsoft Visual C++ 5.0 (or compat)

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

CPP=cl.exe
PERL=perl.exe
FLEX=flex.exe
YACC=bison.exe
MV=move

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

REFDOCDIR= ../../../doc/src/sgml/ref

CPP_PROJ=/nologo $(OPT) /W3 /EHsc /D "WIN32" $(DEBUGDEF) /D "_CONSOLE" /D\
 "_MBCS" /Fp"$(INTDIR)\pg_dump.pch" /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c \
 /I ..\..\include /I ..\..\interfaces\libpq /I ..\..\include\port\win32 \
 /I ..\..\include\port\win32_msvc /I ..\..\backend \
 /D "FRONTEND" /D "_CRT_SECURE_NO_DEPRECATE"

CPP_OBJS=$(INTDIR)/
CPP_SBRS=.

ALL : ..\..\backend\parser\parse.h "..\..\port\pg_config_paths.h" \
 "$(OUTDIR)\pg_dump.exe" "$(OUTDIR)\pg_dumpall.exe" "$(OUTDIR)\pg_restore.exe"

CLEAN :
	-@erase "$(INTDIR)\pg_backup_archiver.obj"
	-@erase "$(INTDIR)\pg_backup_db.obj"
	-@erase "$(INTDIR)\pg_backup_custom.obj"
	-@erase "$(INTDIR)\pg_backup_files.obj"
	-@erase "$(INTDIR)\pg_backup_null.obj"
	-@erase "$(INTDIR)\pg_backup_tar.obj"
	-@erase "$(INTDIR)\dumputils.obj"
	-@erase "$(INTDIR)\common.obj" 
	-@erase "$(INTDIR)\pg_dump_sort.obj" 
	-@erase "$(INTDIR)\keywords.obj" 
	-@erase "$(INTDIR)\exec.obj"
	-@erase "$(INTDIR)\getopt.obj"
	-@erase "$(INTDIR)\getopt_long.obj"
	-@erase "$(INTDIR)\path.obj"
	-@erase "$(INTDIR)\strlcpy.obj"
	-@erase "$(INTDIR)\pgstrcasecmp.obj"
	-@erase "$(INTDIR)\sprompt.obj"
	-@erase "$(INTDIR)\snprintf.obj"
	-@erase "$(INTDIR)\qsort.obj"
#	-@erase "$(INTDIR)\pg_dump.pch"
	-@erase "$(OUTDIR)\pg_dump.obj"
	-@erase "$(OUTDIR)\pg_dump.exe"
	-@erase "$(INTDIR)\pg_dumpall.obj"
	-@erase "$(OUTDIR)\pg_dumpall.exe"
	-@erase "$(INTDIR)\pg_restore.obj"
	-@erase "$(OUTDIR)\pg_restore.exe"
#	-@erase "$(INTDIR)\..\..\port\pg_config_paths.h"
#	-@erase "$(INTDIR)\..\..\backend\parser\parse.h" 
#	-@erase "$(INTDIR)\..\..\backend\parser\gram.c"

LINK32=link.exe
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shfolder.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib wsock32.lib /nologo /subsystem:console /incremental:no
LINK32_FLAGS_DMP= \
 /pdb:"$(OUTDIR)\pg_dump.pdb" /machine:I386 $(LOPT) /out:"$(OUTDIR)\pg_dump.exe"
LINK32_FLAGS_ALL= \
 /pdb:"$(OUTDIR)\pg_dumpall.pdb" /machine:I386 $(LOPT) /out:"$(OUTDIR)\pg_dumpall.exe"
LINK32_FLAGS_RES= \
 /pdb:"$(OUTDIR)\pg_restore.pdb" /machine:I386 $(LOPT) /out:"$(OUTDIR)\pg_restore.exe"

LINK32_OBJS= \
	"$(INTDIR)\pg_backup_archiver.obj" \
	"$(INTDIR)\pg_backup_db.obj" \
	"$(INTDIR)\pg_backup_custom.obj" \
	"$(INTDIR)\pg_backup_files.obj" \
	"$(INTDIR)\pg_backup_null.obj" \
	"$(INTDIR)\pg_backup_tar.obj" \
	"$(INTDIR)\dumputils.obj" \
	"$(INTDIR)\keywords.obj" \
	"$(INTDIR)\exec.obj" \
	"$(INTDIR)\getopt.obj" \
	"$(INTDIR)\getopt_long.obj" \
	"$(INTDIR)\path.obj" \
	"$(INTDIR)\strlcpy.obj" \
	"$(INTDIR)\pgstrcasecmp.obj" \
	"$(INTDIR)\sprompt.obj" \
	"$(INTDIR)\snprintf.obj" \
	"$(INTDIR)\qsort.obj"

LINK32_OBJS_DMP= \
	"$(INTDIR)\common.obj" \
	"$(INTDIR)\pg_dump_sort.obj" \
	"$(INTDIR)\pg_dump.obj"
LINK32_OBJS_RES= "$(INTDIR)\pg_restore.obj"
LINK32_OBJS_ALL= "$(INTDIR)\pg_dumpall.obj"

!IFDEF DEBUG
LINK32_OBJS	= $(LINK32_OBJS) "..\..\interfaces\libpq\Debug\libpqddll.lib"
!ELSE
LINK32_OBJS	= $(LINK32_OBJS) "..\..\interfaces\libpq\Release\libpqdll.lib"
!ENDIF

"..\..\port\pg_config_paths.h": 
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

"$(OUTDIR)\pg_dump.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS) $(LINK32_OBJS_DMP)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_FLAGS_DMP) $(LINK32_OBJS) $(LINK32_OBJS_DMP)
<<

"$(OUTDIR)\pg_dumpall.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS) $(LINK32_OBJS_ALL)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_FLAGS_ALL) $(LINK32_OBJS) $(LINK32_OBJS_ALL)
<<

"$(OUTDIR)\pg_restore.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS) $(LINK32_OBJS_RES)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_FLAGS_RES) $(LINK32_OBJS) $(LINK32_OBJS_RES)
<<

"$(INTDIR)\keywords.obj" : ..\..\backend\parser\keywords.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\backend\parser\keywords.c
<<

"$(INTDIR)\exec.obj" : ..\..\port\exec.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\exec.c
<<

"$(INTDIR)\getopt.obj" : "$(INTDIR)" ..\..\port\getopt.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\getopt.c
<<

"$(INTDIR)\getopt_long.obj" : "$(INTDIR)" ..\..\port\getopt_long.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\getopt_long.c
<<

"$(INTDIR)\path.obj" : "$(INTDIR)" ..\..\port\path.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\path.c
<<

"$(INTDIR)\strlcpy.obj" : "$(INTDIR)" ..\..\port\strlcpy.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\strlcpy.c
<<

"$(INTDIR)\pgstrcasecmp.obj" : ..\..\port\pgstrcasecmp.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\pgstrcasecmp.c
<<

"$(INTDIR)\sprompt.obj" : "$(INTDIR)" ..\..\port\sprompt.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\sprompt.c
<<

"$(INTDIR)\snprintf.obj" : "$(INTDIR)" ..\..\port\snprintf.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\snprintf.c
<<

"$(INTDIR)\qsort.obj" : "$(INTDIR)" ..\..\port\qsort.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\qsort.c
<<

..\..\backend\parser\parse.h : ..\..\backend\parser\gram.y
	$(YACC) -y -d  ..\..\backend\parser\gram.y
	$(MV) ..\..\backend\parser\y.tab.h ..\..\backend\parser\parse.h 
	$(MV) ..\..\backend\parser\y.tab.c ..\..\backend\parser\gram.c

.c{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<


