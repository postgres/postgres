#
# File:			win32w.mak
#
# Description:		psqlodbc Unicode version Makefile for Win32.
#
# Configurations:	UnicodeDebug, Unicode
# Build Types:		ALL, CLEAN
# Usage:		NMAKE /f win32w.mak CFG=[Unicode | UnicodeDebug] [ALL | CLEAN]
#
# Comments:		Created by Dave Page, 2001-02-12
#

!MESSAGE Building the PostgreSQL Unicode Driver for Win32...
!MESSAGE
!IF "$(CFG)" == ""
CFG=Unicode
!MESSAGE No configuration specified. Defaulting to Unicode.
!MESSAGE
!ENDIF 

!IF "$(CFG)" != "Unicode" && "$(CFG)" != "UnicodeDebug"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f win32w.mak CFG=[Unicode | UnicodeDebug] [ALL | CLEAN]
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "Unicode" (Win32 Release DLL)
!MESSAGE "UnicodeDebug" (Win32 Debug DLL)
!MESSAGE 
!ERROR An invalid configuration was specified.
!ENDIF 

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

!IF  "$(CFG)" == "Unicode"

OUTDIR=.\Unicode
OUTDIRBIN=.\Unicode
INTDIR=.\Unicode

ALL : "$(OUTDIRBIN)\psqlodbc.dll"


CLEAN :
	-@erase "$(INTDIR)\bind.obj"
	-@erase "$(INTDIR)\columninfo.obj"
	-@erase "$(INTDIR)\connection.obj"
	-@erase "$(INTDIR)\convert.obj"
	-@erase "$(INTDIR)\dlg_specific.obj"
	-@erase "$(INTDIR)\drvconn.obj"
	-@erase "$(INTDIR)\environ.obj"
	-@erase "$(INTDIR)\execute.obj"
	-@erase "$(INTDIR)\info.obj"
	-@erase "$(INTDIR)\lobj.obj"
	-@erase "$(INTDIR)\win_md5.obj"
	-@erase "$(INTDIR)\misc.obj"
	-@erase "$(INTDIR)\multibyte.obj"
	-@erase "$(INTDIR)\odbcapiw.obj"
	-@erase "$(INTDIR)\odbcapi25w.obj"
	-@erase "$(INTDIR)\win_unicode.obj"
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
	-@erase "$(INTDIR)\odbcapi.obj"
	-@erase "$(INTDIR)\vc60.idb"
	-@erase "$(OUTDIR)\psqlodbc.dll"
	-@erase "$(OUTDIR)\psqlodbc.exp"
	-@erase "$(OUTDIR)\psqlodbc.lib"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP=cl.exe
CPP_PROJ=/nologo /MT /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /D "PSQLODBC_EXPORTS" /D "ODBCVER=0x0250" /D "MULTIBYTE" /D "UNICODE_SUPPORT" /D "DRIVER_CURSOR_IMPLEMENT" /Fp"$(INTDIR)\psqlodbc.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c 

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
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib wsock32.lib /nologo /dll /incremental:no /pdb:"$(OUTDIR)\psqlodbc.pdb" /machine:I386 /def:"psqlodbc_apiw.def" /out:"$(OUTDIRBIN)\psqlodbc.dll" /implib:"$(OUTDIR)\psqlodbc.lib" 
DEF_FILE= "psqlodbc_apiw.def"
LINK32_OBJS= \
	"$(INTDIR)\bind.obj" \
	"$(INTDIR)\columninfo.obj" \
	"$(INTDIR)\connection.obj" \
	"$(INTDIR)\convert.obj" \
	"$(INTDIR)\dlg_specific.obj" \
	"$(INTDIR)\drvconn.obj" \
	"$(INTDIR)\environ.obj" \
	"$(INTDIR)\execute.obj" \
	"$(INTDIR)\info.obj" \
	"$(INTDIR)\lobj.obj" \
	"$(INTDIR)\win_md5.obj" \
	"$(INTDIR)\misc.obj" \
	"$(INTDIR)\multibyte.obj" \
	"$(INTDIR)\odbcapiw.obj" \
	"$(INTDIR)\odbcapi25w.obj" \
	"$(INTDIR)\win_unicode.obj" \
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
	"$(INTDIR)\odbcapi.obj" \
	"$(INTDIR)\psqlodbc.res"

"$(OUTDIRBIN)\psqlodbc.dll" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "UnicodeDebug"

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
	-@erase "$(INTDIR)\info.obj"
	-@erase "$(INTDIR)\lobj.obj"
	-@erase "$(INTDIR)\win_md5.obj"
	-@erase "$(INTDIR)\misc.obj"
	-@erase "$(INTDIR)\multibyte.obj"
	-@erase "$(INTDIR)\odbcapiw.obj"
	-@erase "$(INTDIR)\odbcapi25w.obj"
	-@erase "$(INTDIR)\win_unicode.obj"
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
	-@erase "$(INTDIR)\odbcapi.obj"
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
CPP_PROJ=/nologo /MTd /W3 /Gm /GX /ZI /Od /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /D "PSQLODBC_EXPORTS" /D "ODBCVER=0x0250" /D "MULTIBYTE" /D "UNICODE_SUPPORT" /D "DRIVER_CURSOR_IMPLEMENT" /Fp"$(INTDIR)\psqlodbc.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /GZ /c 

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
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib wsock32.lib /nologo /dll /incremental:yes /pdb:"$(OUTDIR)\psqlodbc.pdb" /debug /machine:I386 /def:"psqlodbc_apiw.def" /out:"$(OUTDIR)\psqlodbc.dll" /implib:"$(OUTDIR)\psqlodbc.lib" /pdbtype:sept 
DEF_FILE= "psqlodbc_apiw.def"
LINK32_OBJS= \
	"$(INTDIR)\bind.obj" \
	"$(INTDIR)\columninfo.obj" \
	"$(INTDIR)\connection.obj" \
	"$(INTDIR)\convert.obj" \
	"$(INTDIR)\dlg_specific.obj" \
	"$(INTDIR)\drvconn.obj" \
	"$(INTDIR)\environ.obj" \
	"$(INTDIR)\execute.obj" \
	"$(INTDIR)\info.obj" \
	"$(INTDIR)\lobj.obj" \
	"$(INTDIR)\win_md5.obj"
	"$(INTDIR)\misc.obj" \
	"$(INTDIR)\multibyte.obj" \
	"$(INTDIR)\odbcapiw.obj" \
	"$(INTDIR)\odbcapi25w.obj" \
	"$(INTDIR)\win_unicode.obj" \
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
	"$(INTDIR)\odbcapi.obj" \
	"$(INTDIR)\psqlodbc.res"

"$(OUTDIR)\psqlodbc.dll" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ENDIF 

!IF "$(CFG)" == "Unicode" || "$(CFG)" == "UnicodeDebug"

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


SOURCE=info.c

"$(INTDIR)\info.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=lobj.c

"$(INTDIR)\lobj.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=misc.c

"$(INTDIR)\misc.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=multibyte.c

"$(INTDIR)\multibyte.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)

SOURCE=odbcapiw.c

"$(INTDIR)\odbcapiw.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)

SOURCE=odbcapi25w.c

"$(INTDIR)\odbcapi25w.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)

SOURCE=win_unicode.c

"$(INTDIR)\win_unicode.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


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

!IF "$(CFG)" == "Unicode"
"$(INTDIR)\psqlodbc.res" : $(SOURCE) "$(INTDIR)"
	$(RSC) /l 0x809 /fo"$(INTDIR)\psqlodbc.res" /d "NDEBUG" /d "MULTIBYTE" $(SOURCE)
!ENDIF

!IF "$(CFG)" == "UnicodeDebug"
"$(INTDIR)\psqlodbc.res" : $(SOURCE) "$(INTDIR)"
	$(RSC) /l 0x809 /fo"$(INTDIR)\psqlodbc.res" /d "_DEBUG" $(SOURCE)
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


SOURCE=win_md5.c

"$(INTDIR)\win_md5.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


SOURCE=odbcapi.c

"$(INTDIR)\odbcapi.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)



!ENDIF 
