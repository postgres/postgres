# Makefile for Microsoft Visual C++ 5.0 (or compat)

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

CPP=cl.exe
PERL=perl.exe

OUTDIR=.\Release
INTDIR=.\Release
REFDOCDIR= ../../../doc/src/sgml/ref
# Begin Custom Macros
OutDir=.\Release
# End Custom Macros

ALL : sql_help.h "$(OUTDIR)\psql.exe"

CLEAN :
	-@erase "$(INTDIR)\command.obj"
	-@erase "$(INTDIR)\common.obj"
	-@erase "$(INTDIR)\help.obj"
	-@erase "$(INTDIR)\input.obj"
	-@erase "$(INTDIR)\stringutils.obj"
	-@erase "$(INTDIR)\mainloop.obj"
	-@erase "$(INTDIR)\copy.obj"
	-@erase "$(INTDIR)\startup.obj"
	-@erase "$(INTDIR)\prompt.obj"
	-@erase "$(INTDIR)\variables.obj"
	-@erase "$(INTDIR)\large_obj.obj"
	-@erase "$(INTDIR)\print.obj"
	-@erase "$(INTDIR)\describe.obj"
	-@erase "$(INTDIR)\tab-complete.obj"
	-@erase "$(INTDIR)\sprompt.obj"
	-@erase "$(INTDIR)\getopt.obj"
	-@erase "$(INTDIR)\getopt_long.obj"
	-@erase "$(INTDIR)\path.obj"
	-@erase "$(INTDIR)\mbprint.obj"
	-@erase "$(INTDIR)\*psql.pch"
	-@erase "$(OUTDIR)\psql.exe"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo /MD /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /D\
 "_MBCS" /Fp"$(INTDIR)\psql.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c \
 /I ..\..\include /I ..\..\interfaces\libpq /D "HAVE_STRDUP" /D "FRONTEND"

CPP_OBJS=.\Release/
CPP_SBRS=.

LINK32=link.exe
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib wsock32.lib /nologo /subsystem:console /incremental:no\
 /pdb:"$(OUTDIR)\psql.pdb" /machine:I386 /out:"$(OUTDIR)\psql.exe" 
LINK32_OBJS= \
	"$(INTDIR)\command.obj" \
	"$(INTDIR)\common.obj" \
	"$(INTDIR)\help.obj" \
	"$(INTDIR)\input.obj" \
	"$(INTDIR)\stringutils.obj" \
	"$(INTDIR)\mainloop.obj" \
	"$(INTDIR)\copy.obj" \
	"$(INTDIR)\startup.obj" \
	"$(INTDIR)\prompt.obj" \
	"$(INTDIR)\variables.obj" \
	"$(INTDIR)\large_obj.obj" \
	"$(INTDIR)\print.obj" \
	"$(INTDIR)\describe.obj" \
	"$(INTDIR)\tab-complete.obj" \
	"$(INTDIR)\sprompt.obj" \
	"$(INTDIR)\getopt.obj" \
	"$(INTDIR)\getopt_long.obj" \
	"$(INTDIR)\path.obj" \
	"$(INTDIR)\mbprint.obj" \
	"..\..\interfaces\libpq\Release\libpqdll.lib"

"$(OUTDIR)\psql.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

"$(OUTDIR)\sprompt.obj" : "$(OUTDIR)" ..\..\port\sprompt.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\sprompt.c
<<

"$(OUTDIR)\getopt.obj" : "$(OUTDIR)" ..\..\port\getopt.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\getopt.c
<<

"$(OUTDIR)\getopt_long.obj" : "$(OUTDIR)" ..\..\port\getopt_long.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\getopt_long.c
<<

"$(OUTDIR)\path.obj" : "$(OUTDIR)" ..\..\port\path.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\path.c
<<

.c{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cpp{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

sql_help.h: create_help.pl
        $(PERL) create_help.pl $(REFDOCDIR) $@

