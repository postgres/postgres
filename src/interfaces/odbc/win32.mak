
#
# File:			win32.mak
#
# Description:		psqlodbc Makefile for Win32.
#
# Configurations:	Debug, Release
# Build Types:		ALL, CLEAN
#
# Comments:		Created by Dave Page, 2001-02-12
#

!MESSAGE Building the PostgreSQL ODBC Driver for Win32...
!MESSAGE
!IF "$(CFG)" == ""
CFG=Release
!MESSAGE No configuration specified. Defaulting to Release.
!MESSAGE
!ENDIF 

!IF "$(CFG)" != "Release" && "$(CFG)" != "Debug" && "$(CFG)" != "MultibyteRelease" && "$(CFG)" != "MultibyteDebug"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f win32.mak CFG=[Release | Debug | MultibyteRelease | MultiByteDebug]
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "Release" (based on "Win32 (x86) Dynamic-Link Library")
!MESSAGE "Debug" (based on "Win32 (x86) Dynamic-Link Library")
!MESSAGE 
!ERROR An invalid configuration is specified.
!ENDIF 

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

!IF  "$(CFG)" == "Release" || "$(CFG)" == "MultibyteRelease"

OUTDIR=.\Release
INTDIR=.\Release
# Begin Custom Macros
OutDir=.\Release
# End Custom Macros

ALL : "$(OUTDIR)\psqlodbc.dll"


CLEAN :
	-@erase "$(INTDIR)\bind.obj"
	-@erase "$(INTDIR)\columninfo.obj"
	-@erase "$(INTDIR)\connection.obj"
	-@erase "$(INTDIR)\convert.obj"
	-@erase "$(INTDIR)\dlg_specific.obj"
	-@erase "$(INTDIR)\drvconn.obj"
	-@erase "$(INTDIR)\environ.obj"
	-@erase "$(INTDIR)\execute.obj"
	-@erase "$(INTDIR)\gpps.obj"
	-@erase "$(INTDIR)\info.obj"
	-@erase "$(INTDIR)\lobj.obj"
	-@erase "$(INTDIR)\misc.obj"
!IF "$(CFG)" == "MultibyteRelease" || "$(CFG)" == "MultibyteDebug" 
	-@erase "$(INTDIR)\multibyte.obj"
!ENDIF
	-@erase "$(INTDIR)\options.obj"
	-@erase "$(INTDIR)\parse.obj"
	-@erase "$(INTDIR)\pgtypes.obj"
	-@erase "$(INTDIR)\psqlodbc.obj"
	-@erase "$(INTDIR)\psqlodbc.res"
	-@erase "$(INTDIR)\qresult.obj"
	-@erase "$(INTDIR)\results.obj"
	-@erase "$(INTDIR)\setup.obj"
	-@erase "$(INTDIR)\socket.obj"
	-@erase "$(INTDIR)\statement.obj"
	-@erase "$(INTDIR)\tuple.obj"
	-@erase "$(INTDIR)\tuplelist.obj"
	-@erase "$(INTDIR)\vc60.idb"
	-@erase "$(OUTDIR)\psqlodbc.dll"
	-@erase "$(OUTDIR)\psqlodbc.exp"
	-@erase "$(OUTDIR)\psqlodbc.lib"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP=cl.exe
!IF "$(CFG)" == "MultibyteRelease" || "$(CFG)" == "MultibyteDebug" 
CPP_PROJ=/nologo /MT /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /D "PSQLODBC_EXPORTS" /D "MULTIBYTE" /Fp"$(INTDIR)\psqlodbc.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c 
!ELSE
CPP_PROJ=/nologo /MT /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /D "PSQLODBC_EXPORTS" /Fp"$(INTDIR)\psqlodbc.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c 
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

MTL=midl.exe
MTL_PROJ=/nologo /D "NDEBUG" /mktyplib203 /win32 
RSC=rc.exe
RSC_PROJ=/l 0x809 /fo"$(INTDIR)\psqlodbc.res" /d "NDEBUG" 
BSC32=bscmake.exe
BSC32_FLAGS=/nologo /o"$(OUTDIR)\psqlodbc.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib wsock32.lib /nologo /dll /incremental:no /pdb:"$(OUTDIR)\psqlodbc.pdb" /machine:I386 /def:"psqlodbc.def" /out:"$(OUTDIR)\psqlodbc.dll" /implib:"$(OUTDIR)\psqlodbc.lib" 
DEF_FILE= "psqlodbc.def"
LINK32_OBJS= \
	"$(INTDIR)\bind.obj" \
	"$(INTDIR)\columninfo.obj" \
	"$(INTDIR)\connection.obj" \
	"$(INTDIR)\convert.obj" \
	"$(INTDIR)\dlg_specific.obj" \
	"$(INTDIR)\drvconn.obj" \
	"$(INTDIR)\environ.obj" \
	"$(INTDIR)\execute.obj" \
	"$(INTDIR)\gpps.obj" \
	"$(INTDIR)\info.obj" \
	"$(INTDIR)\lobj.obj" \
	"$(INTDIR)\misc.obj" \
!IF "$(CFG)" == "MultibyteRelease" || "$(CFG)" == "MultibyteDebug" 
	"$(INTDIR)\multibyte.obj" \
!ENDIF
	"$(INTDIR)\options.obj" \
	"$(INTDIR)\parse.obj" \
	"$(INTDIR)\pgtypes.obj" \
	"$(INTDIR)\psqlodbc.obj" \
	"$(INTDIR)\qresult.obj" \
	"$(INTDIR)\results.obj" \
	"$(INTDIR)\setup.obj" \
	"$(INTDIR)\socket.obj" \
	"$(INTDIR)\statement.obj" \
	"$(INTDIR)\tuple.obj" \
	"$(INTDIR)\tuplelist.obj" \
	"$(INTDIR)\psqlodbc.res"

"$(OUTDIR)\psqlodbc.dll" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "Debug" || "$(CFG)" == "MultibyteDebug"

OUTDIR=.\Debug
INTDIR=.\Debug
# Begin Custom Macros
OutDir=.\Debug
# End Custom Macros

ALL : "$(OUTDIR)\psqlodbc.dll"


CLEAN :
	-@erase "$(INTDIR)\bind.obj"
	-@erase "$(INTDIR)\columninfo.obj"
	-@erase "$(INTDIR)\connection.obj"
	-@erase "$(INTDIR)\convert.obj"
	-@erase "$(INTDIR)\dlg_specific.obj"
	-@erase "$(INTDIR)\drvconn.obj"
	-@erase "$(INTDIR)\environ.obj"
	-@erase "$(INTDIR)\execute.obj"
	-@erase "$(INTDIR)\gpps.obj"
	-@erase "$(INTDIR)\info.obj"
	-@erase "$(INTDIR)\lobj.obj"
	-@erase "$(INTDIR)\misc.obj"
!IF "$(CFG)" == "MultibyteRelease" || "$(CFG)" == "MultibyteDebug" 
	-@erase "$(INTDIR)\multibyte.obj"
!ENDIF
	-@erase "$(INTDIR)\options.obj"
	-@erase "$(INTDIR)\parse.obj"
	-@erase "$(INTDIR)\pgtypes.obj"
	-@erase "$(INTDIR)\psqlodbc.obj"
	-@erase "$(INTDIR)\psqlodbc.res"
	-@erase "$(INTDIR)\qresult.obj"
	-@erase "$(INTDIR)\results.obj"
	-@erase "$(INTDIR)\setup.obj"
	-@erase "$(INTDIR)\socket.obj"
	-@erase "$(INTDIR)\statement.obj"
	-@erase "$(INTDIR)\tuple.obj"
	-@erase "$(INTDIR)\tuplelist.obj"
	-@erase "$(INTDIR)\vc60.idb"
	-@erase "$(INTDIR)\vc60.pdb"
	-@erase "$(OUTDIR)\psqlodbc.dll"
	-@erase "$(OUTDIR)\psqlodbc.exp"
	-@erase "$(OUTDIR)\psqlodbc.ilk"
	-@erase "$(OUTDIR)\psqlodbc.lib"
	-@erase "$(OUTDIR)\psqlodbc.pdb"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP=cl.exe
!IF "$(CFG)" == "MultibyteRelease" || "$(CFG)" == "MultibyteDebug" 
CPP_PROJ=/nologo /MTd /W3 /Gm /GX /ZI /Od /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /D "PSQLODBC_EXPORTS" /D "MULTIBYTE" /Fp"$(INTDIR)\psqlodbc.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /GZ /c 
!ELSE
CPP_PROJ=/nologo /MTd /W3 /Gm /GX /ZI /Od /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /D "PSQLODBC_EXPORTS" /Fp"$(INTDIR)\psqlodbc.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /GZ /c 
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

MTL=midl.exe
MTL_PROJ=/nologo /D "_DEBUG" /mktyplib203 /win32 
RSC=rc.exe
RSC_PROJ=/l 0x809 /fo"$(INTDIR)\psqlodbc.res" /d "_DEBUG" 
BSC32=bscmake.exe
BSC32_FLAGS=/nologo /o"$(OUTDIR)\psqlodbc.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib wsock32.lib /nologo /dll /incremental:yes /pdb:"$(OUTDIR)\psqlodbc.pdb" /debug /machine:I386 /def:"psqlodbc.def" /out:"$(OUTDIR)\psqlodbc.dll" /implib:"$(OUTDIR)\psqlodbc.lib" /pdbtype:sept 
DEF_FILE= "psqlodbc.def"
LINK32_OBJS= \
	"$(INTDIR)\bind.obj" \
	"$(INTDIR)\columninfo.obj" \
	"$(INTDIR)\connection.obj" \
	"$(INTDIR)\convert.obj" \
	"$(INTDIR)\dlg_specific.obj" \
	"$(INTDIR)\drvconn.obj" \
	"$(INTDIR)\environ.obj" \
	"$(INTDIR)\execute.obj" \
	"$(INTDIR)\gpps.obj" \
	"$(INTDIR)\info.obj" \
	"$(INTDIR)\lobj.obj" \
	"$(INTDIR)\misc.obj" \
!IF "$(CFG)" == "MultibyteRelease" || "$(CFG)" == "MultibyteDebug" 
	"$(INTDIR)\multibyte.obj" \
!ENDIF
	"$(INTDIR)\options.obj" \
	"$(INTDIR)\parse.obj" \
	"$(INTDIR)\pgtypes.obj" \
	"$(INTDIR)\psqlodbc.obj" \
	"$(INTDIR)\qresult.obj" \
	"$(INTDIR)\results.obj" \
	"$(INTDIR)\setup.obj" \
	"$(INTDIR)\socket.obj" \
	"$(INTDIR)\statement.obj" \
	"$(INTDIR)\tuple.obj" \
	"$(INTDIR)\tuplelist.obj" \
	"$(INTDIR)\psqlodbc.res"

"$(OUTDIR)\psqlodbc.dll" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ENDIF 

!IF "$(CFG)" == "Release" || "$(CFG)" == "Debug" || "$(CFG)" == "MultibyteRelease" || "$(CFG)" == "MultibyteDebug"

SOURCE=bind.c

"$(INTDIR)\bind.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=columninfo.c

"$(INTDIR)\columninfo.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=connection.c

"$(INTDIR)\connection.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=convert.c

"$(INTDIR)\convert.obj" : $(SOURCE) "$(INTDIR)"

	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=dlg_specific.c

"$(INTDIR)\dlg_specific.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=drvconn.c

"$(INTDIR)\drvconn.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=environ.c

"$(INTDIR)\environ.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=execute.c

"$(INTDIR)\execute.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=gpps.c

"$(INTDIR)\gpps.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=info.c

"$(INTDIR)\info.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=lobj.c

"$(INTDIR)\lobj.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=misc.c

"$(INTDIR)\misc.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)

!IF "$(CFG)" == "MultibyteRelease" || "$(CFG)" == "MultibyteDebug" 
SOURCE=multibyte.c

"$(INTDIR)\multibyte.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF
SOURCE=options.c

"$(INTDIR)\options.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=parse.c

"$(INTDIR)\parse.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=pgtypes.c

"$(INTDIR)\pgtypes.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=psqlodbc.c

"$(INTDIR)\psqlodbc.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=psqlodbc.rc

!IF  "$(CFG)" == "Release" || "$(CFG)" == "MultibyteRelease"


!IF "$(CFG)" == "Release"
"$(INTDIR)\psqlodbc.res" : $(SOURCE) "$(INTDIR)"
	$(RSC) /l 0x809 /fo"$(INTDIR)\psqlodbc.res" /d "NDEBUG" $(SOURCE)
!ELSEIF "$(CFG)" == "MultibyteRelease"
"$(INTDIR)\psqlodbc.res" : $(SOURCE) "$(INTDIR)"
	$(RSC) /l 0x809 /fo"$(INTDIR)\psqlodbc.res" /d "NDEBUG" /d "MULTIBYTE" $(SOURCE)
!ENDIF


!ELSEIF  "$(CFG)" == "Debug" || "$(CFG)" == "MultibyteDebug"

!IF "$(CFG)" == "Debug"
"$(INTDIR)\psqlodbc.res" : $(SOURCE) "$(INTDIR)"
	$(RSC) /l 0x809 /fo"$(INTDIR)\psqlodbc.res" /d "_DEBUG" $(SOURCE)
!ELSEIF "$(CFG)" == "MultibyteDebug"
"$(INTDIR)\psqlodbc.res" : $(SOURCE) "$(INTDIR)"
	$(RSC) /l 0x809 /fo"$(INTDIR)\psqlodbc.res" /d "_DEBUG" /d "MULTIBYTE" $(SOURCE)
!ENDIF

!ENDIF 

SOURCE=qresult.c

"$(INTDIR)\qresult.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=results.c

"$(INTDIR)\results.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=setup.c

"$(INTDIR)\setup.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=socket.c

"$(INTDIR)\socket.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=statement.c

"$(INTDIR)\statement.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=tuple.c

"$(INTDIR)\tuple.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=tuplelist.c

"$(INTDIR)\tuplelist.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)



!ENDIF 
