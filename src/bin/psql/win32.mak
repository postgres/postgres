# Makefile for Microsoft Visual C++ 5.0 (or compat)

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

CPP=cl.exe

OUTDIR=.\Release
INTDIR=.\Release
# Begin Custom Macros
OutDir=.\Release
# End Custom Macros

ALL : "$(OUTDIR)\psql.exe"

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
	-@erase "$(INTDIR)\getopt.obj"
	-@erase "$(INTDIR)\vc50.idb"
	-@erase "$(OUTDIR)\psql.exe"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo /ML /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /D\
 "_MBCS" /Fp"$(INTDIR)\psql.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c \
 /I ..\..\include /I ..\..\interfaces\libpq /D "HAVE_STRDUP"

!IFDEF        MULTIBYTE
!IFNDEF MBFLAGS
MBFLAGS="-DMULTIBYTE=$(MULTIBYTE)"
!ENDIF
CPP_PROJ=$(MBFLAGS) $(CPP_PROJ)
!ENDIF

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
	"$(INTDIR)\getopt.obj" \
	"..\..\interfaces\libpq\Release\libpqdll.lib"

"$(OUTDIR)\psql.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

"$(OUTDIR)\getopt.obj" : "$(OUTDIR)" ..\..\utils\getopt.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\utils\getopt.c
<<

.c{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cpp{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<
