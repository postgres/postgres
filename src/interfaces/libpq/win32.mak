# Makefile for Microsoft Visual C++ 5.0 (or compat)

# Will build a Win32 static library (non-debug) libpq.lib
#        and a Win32 dynamic library (non-debug) libpq.dll with import library libpqdll.lib


!MESSAGE Building the Win32 static library...
!MESSAGE
!IF "$(CFG)" == ""
CFG=Release
!MESSAGE No configuration specified. Defaulting to Release.
!MESSAGE
!ELSE
!MESSAGE Configuration "$(CFG)"
!MESSAGE
!ENDIF

!IF "$(CFG)" != "Release" && "$(CFG)" != "MultibyteRelease"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE
!MESSAGE NMAKE /f win32.mak CFG=[Release | MultibyteRelease ]
!MESSAGE
!MESSAGE Possible choices for configuration are:
!MESSAGE
!MESSAGE "Release" (Win32 Release DLL)
!MESSAGE "MultibyteRelease" (Win32 Release DLL with Multibyte support)
!MESSAGE
!ERROR An invalid configuration was specified.
!ENDIF


!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

!IF "$(CFG)" == "MultibyteRelease"
MULTIBYTE=1
!ENDIF

CPP=cl.exe
RSC=rc.exe

OUTDIR=.\Release
INTDIR=.\Release

# Begin Custom Macros
OutDir=.\Release
# End Custom Macros

ALL : "$(OUTDIR)\libpq.lib" "$(OUTDIR)\libpq.dll" 

CLEAN :
	-@erase "$(INTDIR)\dllist.obj"
	-@erase "$(INTDIR)\md5.obj"
	-@erase "$(INTDIR)\fe-auth.obj"
	-@erase "$(INTDIR)\fe-connect.obj"
	-@erase "$(INTDIR)\fe-exec.obj"
	-@erase "$(INTDIR)\fe-lobj.obj"
	-@erase "$(INTDIR)\fe-misc.obj"
	-@erase "$(INTDIR)\fe-print.obj"
	-@erase "$(INTDIR)\pqexpbuffer.obj"
	-@erase "$(OUTDIR)\libpqdll.obj"
	-@erase "$(OUTDIR)\libpq.lib"
	-@erase "$(OUTDIR)\libpq.dll"
	-@erase "$(OUTDIR)\libpq.res"
	-@erase "*.pch"
	-@erase "$(OUTDIR)\libpq.pch"
	-@erase "$(OUTDIR)\libpqdll.exp"
	-@erase "$(OUTDIR)\libpqdll.lib"
!IFDEF MULTIBYTE
	-@erase "$(INTDIR)\wchar.obj"
	-@erase "$(INTDIR)\encnames.obj"
!ENDIF

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo /MD /W3 /GX /O2 /I "..\..\include" /D "FRONTEND" /D "NDEBUG" /D\
 "WIN32" /D "_WINDOWS" /Fp"$(INTDIR)\libpq.pch" /YX\
 /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c  /D "HAVE_VSNPRINTF" /D "HAVE_STRDUP"

!IFDEF MULTIBYTE
!IFNDEF	MBFLAGS
MBFLAGS="-DMULTIBYTE=$(MULTIBYTE)"
!ENDIF
CPP_PROJ = $(CPP_PROJ) $(MBFLAGS)
!ENDIF

CPP_OBJS=.\Release/
CPP_SBRS=.
	
LIB32=link.exe -lib
LIB32_FLAGS=/nologo /out:"$(OUTDIR)\libpq.lib" 
LIB32_OBJS= \
	"$(INTDIR)\dllist.obj" \
	"$(INTDIR)\md5.obj" \
	"$(INTDIR)\fe-auth.obj" \
	"$(INTDIR)\fe-connect.obj" \
	"$(INTDIR)\fe-exec.obj" \
	"$(INTDIR)\fe-lobj.obj" \
	"$(INTDIR)\fe-misc.obj" \
	"$(INTDIR)\fe-print.obj" \
	"$(INTDIR)\pqexpbuffer.obj"

!IFDEF MULTIBYTE
LIB32_OBJS = $(LIB32_OBJS) "$(INTDIR)\wchar.obj" "$(INTDIR)\encnames.obj"
!ENDIF

RSC_PROJ=/l 0x409 /fo"$(INTDIR)\libpq.res"

LINK32=link.exe
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib wsock32.lib\
 odbccp32.lib /nologo /subsystem:windows /dll /incremental:no\
 /pdb:"$(OUTDIR)\libpqdll.pdb" /machine:I386 /out:"$(OUTDIR)\libpq.dll"\
 /implib:"$(OUTDIR)\libpqdll.lib"  /def:libpqdll.def
LINK32_OBJS= \
	"$(INTDIR)\libpqdll.obj" \
	"$(OUTDIR)\libpq.lib" \
	"$(OUTDIR)\libpq.res"


"$(OUTDIR)\libpq.lib" : "$(OUTDIR)" $(DEF_FILE) $(LIB32_OBJS)
    $(LIB32) @<<
  $(LIB32_FLAGS) $(DEF_FLAGS) $(LIB32_OBJS)
<<

"$(INTDIR)\libpq.res" : "$(INTDIR)" libpq.rc
    $(RSC) $(RSC_PROJ) libpq.rc


"$(OUTDIR)\libpq.dll" : "$(OUTDIR)" "$(OUTDIR)\libpqdll.obj" "$(INTDIR)\libpqdll.obj" "$(INTDIR)\libpq.res"
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<


"$(OUTDIR)\dllist.obj" : ..\..\backend\lib\dllist.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\backend\lib\dllist.c
<<

    
"$(OUTDIR)\md5.obj" : ..\..\backend\libpq\md5.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\backend\libpq\md5.c
<<

    
!IFDEF MULTIBYTE
"$(INTDIR)\wchar.obj" : ..\..\backend\utils\mb\wchar.c
    $(CPP) @<<
    $(CPP_PROJ) /I "." ..\..\backend\utils\mb\wchar.c
<<
!ENDIF


!IFDEF MULTIBYTE
"$(INTDIR)\encnames.obj" : ..\..\backend\utils\mb\encnames.c
    $(CPP) @<<
    $(CPP_PROJ) /I "." ..\..\backend\utils\mb\encnames.c
<<
!ENDIF


.c{$(CPP_OBJS)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $<
<<

.cpp{$(CPP_OBJS)}.obj::
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

.cxx{$(CPP_SBRS)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<
