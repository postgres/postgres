# Makefile for Microsoft Visual C++ 6.0 (or compat)

# Will build a Win32 static library (non-debug) libpq++.lib
#        and a Win32 dynamic library (non-debug) libpq++.dll with import library libpq++dll.lib


!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

.SUFFIXES : .cc

CPP=cl.exe
RSC=rc.exe

OUTDIR=.\Release
INTDIR=.\Release
# Begin Custom Macros
OutDir=.\Release
# End Custom Macros

ALL : "$(OUTDIR)\libpq++.dll" "$(OUTDIR)\libpq++.lib"

CLEAN :
	-@erase "$(INTDIR)\pgconnection.obj"
	-@erase "$(INTDIR)\pgcursordb.obj"
	-@erase "$(INTDIR)\pgdatabase.obj"
	-@erase "$(INTDIR)\pglobject.obj"
	-@erase "$(INTDIR)\pgtransdb.obj"
	-@erase "$(OUTDIR)\libpq++.lib"
	-@erase "$(OUTDIR)\libpq++.dll"
	-@erase "$(OUTDIR)\libpq++dll.exp"
	-@erase "$(OUTDIR)\libpq++dll.lib"
	-@erase "$(OUTDIR)\libpq++dll.res"
	-@erase "*.pch"
	-@erase "$(OUTDIR)\libpq++.pch"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo /TP /MD /W3 /GX /O2 /I "..\..\include" /I "..\libpq" /D "NDEBUG" /D\
 "WIN32" /D "_WINDOWS" /Fp"$(INTDIR)\libpq++.pch" /YX\
 /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c  /D "HAVE_VSNPRINTF" /D "HAVE_STRDUP"

CPP_OBJS=.\Release/
CPP_SBRS=.
	
LIB32=link.exe -lib
LIB32_FLAGS=/nologo /out:"$(OUTDIR)\libpq++.lib" 
LIB32_OBJS= \
	"$(OUTDIR)\pgconnection.obj" \
	"$(OUTDIR)\pgcursordb.obj" \
	"$(OUTDIR)\pgdatabase.obj" \
	"$(OUTDIR)\pglobject.obj" \
	"$(OUTDIR)\pgtransdb.obj" 

RSC_PROJ=/l 0x409 /fo"$(INTDIR)\libpq++dll.res"

LINK32=link.exe
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib wsock32.lib\
 odbccp32.lib libpq.lib msvcrt.lib /nologo /subsystem:windows /dll /incremental:no\
 /pdb:"$(OUTDIR)\libpq++.pdb" /machine:I386 /out:"$(OUTDIR)\libpq++.dll"\
 /implib:"$(OUTDIR)\libpq++dll.lib" /libpath:"..\libpq\release"
LINK32_OBJS= \
	"$(OUTDIR)\pgconnection.obj" \
	"$(OUTDIR)\pgcursordb.obj" \
	"$(OUTDIR)\pgdatabase.obj" \
	"$(OUTDIR)\pglobject.obj" \
	"$(OUTDIR)\pgtransdb.obj" \
	"$(OUTDIR)\libpq++dll.res"

"$(INTDIR)\libpq++dll.res" : "$(INTDIR)" libpq++dll.rc
    $(RSC) $(RSC_PROJ) libpq++dll.rc

"$(OUTDIR)\libpq++.lib" : "$(OUTDIR)" $(LIB32_OBJS)
	$(LIB32) @<<
	$(LIB32_FLAGS) $(LIB32_OBJS)
<<

"$(OUTDIR)\libpq++.dll" : "$(OUTDIR)" $(LINK32_OBJS)
    $(LINK32) @<<
	$(LINK32_FLAGS) $(LINK32_OBJS)
<<

.c{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $<
<<

.cpp{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cc{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $<
<<	

.cxx{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.c{$(CPP_SBRS)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cpp{$(CPP_SBRS)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cc{$(CPP_SBRS)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cxx{$(CPP_SBRS)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<
