# Makefile for Borland C++ 5.5

# Will build a Win32 static library libpq.lib
#        and a Win32 dynamic library libpq.dll with import library libpqdll.lib

# Borland C++ base install directory goes here
# BCB=c:\Borland\Bcc55

!IF "$(BCB)" == ""
!MESSAGE You must edit bcc32.mak and define BCB at the top
!ERROR misssing BCB
!ENDIF

!IF "$(__NMAKE__)" == ""
!MESSAGE You must use the -N compatibility flag, e.g. make -N -f bcc32.make
!ERROR missing -N
!ENDIF

!MESSAGE Building the Win32 DLL and Static Library...
!MESSAGE
!IF "$(CFG)" == ""
CFG=Release
!MESSAGE No configuration specified. Defaulting to Release.
!MESSAGE
!ELSE
!MESSAGE Configuration "$(CFG)"
!MESSAGE
!ENDIF

!IF "$(CFG)" != "Release" && "$(CFG)" != "Debug"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running MAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE
!MESSAGE make -N -DCFG=[Release | Debug] -f bcc32.mak
!MESSAGE
!MESSAGE Possible choices for configuration are:
!MESSAGE
!MESSAGE "Release" (Win32 Release DLL and Static Library)
!MESSAGE "Debug" (Win32 Debug DLL and Static Library)
!MESSAGE
!ERROR An invalid configuration was specified.
!ENDIF

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

!IF "$(CFG)" == "Debug"
DEBUG=1
OUTDIR=.\Debug
INTDIR=.\Debug
!ELSE
OUTDIR=.\Release
INTDIR=.\Release
!ENDIF

OUTFILENAME=blibpq

USERDEFINES=FRONTEND;NDEBUG;WIN32;_WINDOWS

CPP=bcc32.exe
CPP_PROJ = -I..\..\include\port\win32_msvc;$(BCB)\include;..\..\include;..\..\include\port\win32;..\..\port -n"$(INTDIR)" -WD -c -D$(USERDEFINES) -tWM \
		-a8 -X -w-use -w-par -w-pia -w-csu -w-aus -w-ccc

!IFDEF DEBUG
CPP_PROJ	= $(CPP_PROJ) -Od -r- -k -v -y -vi- -D_DEBUG
!else
CPP_PROJ	= $(CPP_PROJ) -O -Oi -OS -DNDEBUG
!endif

ALL : config "$(OUTDIR)" "$(OUTDIR)\blibpq.dll" "$(OUTDIR)\blibpq.lib"

CLEAN :
	-@erase "$(INTDIR)\getaddrinfo.obj"
	-@erase "$(INTDIR)\pgstrcasecmp.obj"
	-@erase "$(INTDIR)\thread.obj"
	-@erase "$(INTDIR)\inet_aton.obj"
	-@erase "$(INTDIR)\crypt.obj"
	-@erase "$(INTDIR)\noblock.obj"
	-@erase "$(INTDIR)\md5.obj"
	-@erase "$(INTDIR)\ip.obj"
	-@erase "$(INTDIR)\fe-auth.obj"
	-@erase "$(INTDIR)\fe-protocol2.obj"
	-@erase "$(INTDIR)\fe-protocol3.obj"
	-@erase "$(INTDIR)\fe-connect.obj"
	-@erase "$(INTDIR)\fe-exec.obj"
	-@erase "$(INTDIR)\fe-lobj.obj"
	-@erase "$(INTDIR)\fe-misc.obj"
	-@erase "$(INTDIR)\fe-print.obj"
	-@erase "$(INTDIR)\fe-secure.obj"
	-@erase "$(INTDIR)\libpq-events.obj"
	-@erase "$(INTDIR)\pqexpbuffer.obj"
	-@erase "$(INTDIR)\pqsignal.obj"
	-@erase "$(INTDIR)\win32.obj"
	-@erase "$(INTDIR)\wchar.obj"
	-@erase "$(INTDIR)\encnames.obj"
	-@erase "$(INTDIR)\pthread-win32.obj"
	-@erase "$(INTDIR)\snprintf.obj"
	-@erase "$(INTDIR)\strlcpy.obj"
	-@erase "$(INTDIR)\dirent.obj"
	-@erase "$(INTDIR)\dirmod.obj"
	-@erase "$(INTDIR)\pgsleep.obj"
	-@erase "$(INTDIR)\open.obj"
	-@erase "$(INTDIR)\win32error.obj"
	-@erase "$(OUTDIR)\$(OUTFILENAME).lib"
	-@erase "$(OUTDIR)\$(OUTFILENAME)dll.lib"
	-@erase "$(OUTDIR)\libpq.res"
	-@erase "$(OUTDIR)\$(OUTFILENAME).dll"
	-@erase "$(OUTDIR)\$(OUTFILENAME).tds"
	-@erase "$(INTDIR)\pg_config_paths.h"


LIB32=tlib.exe
LIB32_FLAGS= 
LIB32_OBJS= \
	"$(INTDIR)\win32.obj" \
	"$(INTDIR)\getaddrinfo.obj" \
	"$(INTDIR)\pgstrcasecmp.obj" \
	"$(INTDIR)\thread.obj" \
	"$(INTDIR)\inet_aton.obj" \
	"$(INTDIR)\crypt.obj" \
	"$(INTDIR)\noblock.obj" \
	"$(INTDIR)\md5.obj" \
	"$(INTDIR)\ip.obj" \
	"$(INTDIR)\fe-auth.obj" \
	"$(INTDIR)\fe-protocol2.obj" \
	"$(INTDIR)\fe-protocol3.obj" \
	"$(INTDIR)\fe-connect.obj" \
	"$(INTDIR)\fe-exec.obj" \
	"$(INTDIR)\fe-lobj.obj" \
	"$(INTDIR)\fe-misc.obj" \
	"$(INTDIR)\fe-print.obj" \
	"$(INTDIR)\fe-secure.obj" \
	"$(INTDIR)\libpq-events.obj" \
	"$(INTDIR)\pqexpbuffer.obj" \
	"$(INTDIR)\pqsignal.obj" \
	"$(INTDIR)\wchar.obj" \
	"$(INTDIR)\encnames.obj" \
	"$(INTDIR)\snprintf.obj" \
	"$(INTDIR)\strlcpy.obj" \
	"$(INTDIR)\dirent.obj" \
	"$(INTDIR)\dirmod.obj" \
	"$(INTDIR)\pgsleep.obj" \
	"$(INTDIR)\open.obj" \
	"$(INTDIR)\win32error.obj" \
	"$(INTDIR)\pthread-win32.obj"


config: ..\..\include\pg_config.h ..\..\include\pg_config_os.h pg_config_paths.h

..\..\include\pg_config.h: ..\..\include\pg_config.h.win32
	copy ..\..\include\pg_config.h.win32 ..\..\include\pg_config.h

..\..\include\pg_config_os.h: ..\..\include\port\win32.h
	copy ..\..\include\port\win32.h ..\..\include\pg_config_os.h

# Have to use \# so # isn't treated as a comment, but MSVC doesn't like this
pg_config_paths.h: bcc32.mak
	echo \#define SYSCONFDIR "" > pg_config_paths.h

"$(OUTDIR)" :
	@if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

RSC=brcc32.exe
RSC_PROJ=-l 0x409 -i$(BCB)\include -fo"$(INTDIR)\libpq.res"

LINK32=ilink32.exe
LINK32_FLAGS = -Gn -L$(BCB)\lib;$(INTDIR); -x -Tpd -v

# @<< is a Response file, http://www.opussoftware.com/tutorial/TutMakefile.htm

"$(OUTDIR)\blibpq.dll": "$(OUTDIR)\blibpq.lib" "$(INTDIR)\libpq.res" blibpqdll.def 
	$(LINK32) @<<
	$(LINK32_FLAGS) +
	c0d32.obj , +
	$@,, +
	"$(OUTDIR)\blibpq.lib" import32.lib cw32mt.lib, +
	blibpqdll.def,"$(INTDIR)\libpq.res"
<<
	implib -w "$(OUTDIR)\blibpqdll.lib" blibpqdll.def $@

"$(INTDIR)\libpq.res" : "$(INTDIR)" libpq-dist.rc
	$(RSC) $(RSC_PROJ) libpq-dist.rc

"$(OUTDIR)\blibpq.lib": $(LIB32_OBJS)
	$(LIB32) $@ @<<
+-"$(**: =" &^
+-")"
<<


"$(INTDIR)\getaddrinfo.obj" : ..\..\port\getaddrinfo.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\getaddrinfo.c
<<

"$(INTDIR)\pgstrcasecmp.obj" : ..\..\port\pgstrcasecmp.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\pgstrcasecmp.c
<<

"$(INTDIR)\thread.obj" : ..\..\port\thread.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\thread.c
<<

"$(INTDIR)\inet_aton.obj" : ..\..\port\inet_aton.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\inet_aton.c
<<

"$(INTDIR)\crypt.obj" : ..\..\port\crypt.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\crypt.c
<<

"$(INTDIR)\noblock.obj" : ..\..\port\noblock.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\noblock.c
<<

"$(INTDIR)\md5.obj" : ..\..\backend\libpq\md5.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\backend\libpq\md5.c
<<

"$(INTDIR)\ip.obj" : ..\..\backend\libpq\ip.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\backend\libpq\ip.c
<<

"$(INTDIR)\wchar.obj" : ..\..\backend\utils\mb\wchar.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\backend\utils\mb\wchar.c
<<


"$(INTDIR)\encnames.obj" : ..\..\backend\utils\mb\encnames.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\backend\utils\mb\encnames.c
<<

"$(INTDIR)\snprintf.obj" : ..\..\port\snprintf.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\port\snprintf.c
<<

"$(INTDIR)\strlcpy.obj" : ..\..\port\strlcpy.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\port\strlcpy.c
<<

"$(INTDIR)\dirent.obj" : ..\..\port\dirent.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\port\dirent.c
<<

"$(INTDIR)\dirmod.obj" : ..\..\port\dirmod.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\port\dirmod.c
<<

"$(INTDIR)\pgsleep.obj" : ..\..\port\pgsleep.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\port\pgsleep.c
<<

"$(INTDIR)\open.obj" : ..\..\port\open.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\port\open.c
<<

"$(INTDIR)\win32error.obj" : ..\..\port\win32error.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\port\win32error.c
<<


.c.obj:
	$(CPP) $(CPP_PROJ) $<
