# Microsoft Developer Studio Generated NMAKE File, Based on libpgtcl_REL7_1_STABLE.dsp
!IF "$(CFG)" == ""
CFG=libpgtcl - Win32 Release
!MESSAGE No configuration specified. Defaulting to libpgtcl - Win32 Release.
!ENDIF 

!IF "$(CFG)" != "libpgtcl - Win32 Release" && "$(CFG)" != "libpgtcl - Win32 Debug"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "libpgtcl.mak" CFG="libpgtcl - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "libpgtcl - Win32 Release" (based on "Win32 (x86) Dynamic-Link Library")
!MESSAGE "libpgtcl - Win32 Debug" (based on "Win32 (x86) Dynamic-Link Library")
!MESSAGE 
!ERROR An invalid configuration is specified.
!ENDIF 

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

CPP=cl.exe
MTL=midl.exe
RSC=rc.exe

TCLBASE=\usr\local\tcltk833
PGINCLUDE=/I ..\..\include /I ..\libpq /I $(TCLBASE)\include

!IF  "$(CFG)" == "libpgtcl - Win32 Release"

OUTDIR=.\Release
INTDIR=.\Release
# Begin Custom Macros
OutDir=.\Release
# End Custom Macros

ALL : "$(OUTDIR)\libpgtcl.dll" "$(OUTDIR)\libpgtcl.bsc"


CLEAN :
	-@erase "$(INTDIR)\pgtcl.obj"
	-@erase "$(INTDIR)\pgtcl.sbr"
	-@erase "$(INTDIR)\pgtclCmds.obj"
	-@erase "$(INTDIR)\pgtclCmds.sbr"
	-@erase "$(INTDIR)\pgtclId.obj"
	-@erase "$(INTDIR)\pgtclId.sbr"
	-@erase "$(INTDIR)\vc60.idb"
	-@erase "$(OUTDIR)\libpgtcl.dll"
	-@erase "$(OUTDIR)\libpgtcl.exp"
	-@erase "$(OUTDIR)\libpgtcl.lib"
	-@erase "$(OUTDIR)\libpgtcl.bsc"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo /MT /W3 /GX /O2 $(PGINCLUDE) /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /FR"$(INTDIR)\\" /Fp"$(INTDIR)\libpgtcl.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c 
MTL_PROJ=/nologo /D "NDEBUG" /mktyplib203 /win32 
BSC32=bscmake.exe
BSC32_FLAGS=/nologo /o"$(OUTDIR)\libpgtcl.bsc" 
BSC32_SBRS= \
	"$(INTDIR)\pgtcl.sbr" \
	"$(INTDIR)\pgtclCmds.sbr" \
	"$(INTDIR)\pgtclId.sbr"

"$(OUTDIR)\libpgtcl.bsc" : "$(OUTDIR)" $(BSC32_SBRS)
    $(BSC32) @<<
  $(BSC32_FLAGS) $(BSC32_SBRS)
<<

LINK32=link.exe
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib tcl83.lib libpq.lib /nologo /dll /incremental:no /pdb:"$(OUTDIR)\libpgtcl.pdb" /machine:I386 /def:".\libpgtcl.def" /out:"$(OUTDIR)\libpgtcl.dll" /implib:"$(OUTDIR)\libpgtcl.lib" /libpath:"$(TCLBASE)\lib" /libpath:"..\libpq\Release" 
DEF_FILE= \
	".\libpgtcl.def"
LINK32_OBJS= \
	"$(INTDIR)\pgtcl.obj" \
	"$(INTDIR)\pgtclCmds.obj" \
	"$(INTDIR)\pgtclId.obj"

"$(OUTDIR)\libpgtcl.dll" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "libpgtcl - Win32 Debug"

OUTDIR=.\Debug
INTDIR=.\Debug
# Begin Custom Macros
OutDir=.\Debug
# End Custom Macros

ALL : "$(OUTDIR)\libpgtcl.dll" "$(OUTDIR)\libpgtcl.bsc"


CLEAN :
	-@erase "$(INTDIR)\pgtcl.obj"
	-@erase "$(INTDIR)\pgtcl.sbr"
	-@erase "$(INTDIR)\pgtclCmds.obj"
	-@erase "$(INTDIR)\pgtclCmds.sbr"
	-@erase "$(INTDIR)\pgtclId.obj"
	-@erase "$(INTDIR)\pgtclId.sbr"
	-@erase "$(INTDIR)\vc60.idb"
	-@erase "$(INTDIR)\vc60.pdb"
	-@erase "$(OUTDIR)\libpgtcl.dll"
	-@erase "$(OUTDIR)\libpgtcl.exp"
	-@erase "$(OUTDIR)\libpgtcl.ilk"
	-@erase "$(OUTDIR)\libpgtcl.lib"
	-@erase "$(OUTDIR)\libpgtcl.pdb"
	-@erase "$(OUTDIR)\libpgtcl.bsc"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo /MTd /W3 /Gm /GX /ZI /Od $(PGINCLUDE) /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /FR"$(INTDIR)\\" /Fp"$(INTDIR)\libpgtcl.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /GZ /c 
MTL_PROJ=/nologo /D "_DEBUG" /mktyplib203 /win32 
BSC32=bscmake.exe
BSC32_FLAGS=/nologo /o"$(OUTDIR)\libpgtcl.bsc" 
BSC32_SBRS= \
	"$(INTDIR)\pgtcl.sbr" \
	"$(INTDIR)\pgtclCmds.sbr" \
	"$(INTDIR)\pgtclId.sbr"

"$(OUTDIR)\libpgtcl.bsc" : "$(OUTDIR)" $(BSC32_SBRS)
    $(BSC32) @<<
  $(BSC32_FLAGS) $(BSC32_SBRS)
<<

LINK32=link.exe
LINK32_FLAGS=tcl83.lib libpq.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /dll /incremental:yes /pdb:"$(OUTDIR)\libpgtcl.pdb" /debug /machine:I386 /def:".\libpgtcl.def" /out:"$(OUTDIR)\libpgtcl.dll" /implib:"$(OUTDIR)\libpgtcl.lib" /pdbtype:sept /libpath:"$(TCLBASE)\lib" /libpath:"..\libpq\Debug" 
DEF_FILE= \
	".\libpgtcl.def"
LINK32_OBJS= \
	"$(INTDIR)\pgtcl.obj" \
	"$(INTDIR)\pgtclCmds.obj" \
	"$(INTDIR)\pgtclId.obj"

"$(OUTDIR)\libpgtcl.dll" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ENDIF 

.c{$(INTDIR)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cpp{$(INTDIR)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cxx{$(INTDIR)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.c{$(INTDIR)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cpp{$(INTDIR)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cxx{$(INTDIR)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

!IF "$(CFG)" == "libpgtcl - Win32 Release" || "$(CFG)" == "libpgtcl - Win32 Debug"
SOURCE=pgtcl.c

"$(INTDIR)\pgtcl.obj"	"$(INTDIR)\pgtcl.sbr" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=pgtclCmds.c

"$(INTDIR)\pgtclCmds.obj"	"$(INTDIR)\pgtclCmds.sbr" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=pgtclId.c

"$(INTDIR)\pgtclId.obj"	"$(INTDIR)\pgtclId.sbr" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)



!ENDIF 

