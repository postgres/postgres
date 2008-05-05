# Makefile for Microsoft Visual C++ 5.0 (or compat)

# Will build a Win32 static library libpq(d).lib
#        and a Win32 dynamic library libpq(d).dll with import library libpq(d)dll.lib
# USE_SSL=1 will compile with OpenSSL
# DEBUG=1 compiles with debugging symbols
# ENABLE_THREAD_SAFETY=1 compiles with threading enabled

!MESSAGE Building the Win32 static library...
!MESSAGE

!IFDEF DEBUG
OPT=/Od /Zi /MDd
LOPT=/DEBUG
DEBUGDEF=/D _DEBUG
OUTFILENAME=libpqd
!ELSE
OPT=/O2 /MD
LOPT=
DEBUGDEF=/D NDEBUG
OUTFILENAME=libpq
!ENDIF

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

CPP=cl.exe
RSC=rc.exe

!IFDEF DEBUG
OUTDIR=.\Debug
INTDIR=.\Debug
CPP_OBJS=.\Debug/
!ELSE
OUTDIR=.\Release
INTDIR=.\Release
CPP_OBJS=.\Release/
!ENDIF


ALL : config "$(OUTDIR)\$(OUTFILENAME).lib" "$(OUTDIR)\$(OUTFILENAME).dll"

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
	-@erase "$(INTDIR)\pqexpbuffer.obj"
	-@erase "$(INTDIR)\pqsignal.obj"
	-@erase "$(OUTDIR)\win32.obj"
	-@erase "$(INTDIR)\wchar.obj"
	-@erase "$(INTDIR)\encnames.obj"
	-@erase "$(INTDIR)\pthread-win32.obj"
	-@erase "$(INTDIR)\snprintf.obj"
	-@erase "$(INTDIR)\strlcpy.obj"
	-@erase "$(INTDIR)\dirent.obj"
	-@erase "$(INTDIR)\dirmod.obj"
	-@erase "$(INTDIR)\pgsleep.obj"
	-@erase "$(INTDIR)\open.obj"
	-@erase "$(OUTDIR)\$(OUTFILENAME).lib"
	-@erase "$(OUTDIR)\$(OUTFILENAME)dll.lib"
	-@erase "$(OUTDIR)\libpq.res"
	-@erase "$(OUTDIR)\$(OUTFILENAME).dll"
	-@erase "$(OUTDIR)\$(OUTFILENAME)dll.exp"
	-@erase "$(INTDIR)\pg_config_paths.h"


LIB32=link.exe -lib
LIB32_FLAGS=$(LOPT) /nologo /out:"$(OUTDIR)\$(OUTFILENAME).lib" 
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
	"$(INTDIR)\pthread-win32.obj"


config: ..\..\include\pg_config.h pg_config_paths.h

..\..\include\pg_config.h: ..\..\include\pg_config.h.win32
	copy ..\..\include\pg_config.h.win32 ..\..\include\pg_config.h

pg_config_paths.h: win32.mak
	echo #define SYSCONFDIR "" > pg_config_paths.h

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo /W3 /EHsc $(OPT) /I "..\..\include" /I "..\..\include\port\win32" /I "..\..\include\port\win32_msvc" /I "..\..\port" /I. /D "FRONTEND" $(DEBUGDEF) /D\
 "WIN32" /D "_WINDOWS" /Fp"$(INTDIR)\libpq.pch" \
 /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c  \
 /D "_CRT_SECURE_NO_DEPRECATE"

!IFDEF USE_SSL
CPP_PROJ=$(CPP_PROJ) /D USE_SSL
SSL_LIBS=ssleay32.lib libeay32.lib gdi32.lib
!ENDIF

!IFDEF ENABLE_THREAD_SAFETY
CPP_PROJ=$(CPP_PROJ) /D ENABLE_THREAD_SAFETY
!ENDIF

CPP_SBRS=.

RSC_PROJ=/l 0x409 /fo"$(INTDIR)\libpq.res"

LINK32=link.exe
LINK32_FLAGS=kernel32.lib user32.lib advapi32.lib shfolder.lib wsock32.lib $(SSL_LIBS)  \
 /nologo /subsystem:windows /dll $(LOPT) /incremental:no\
 /pdb:"$(OUTDIR)\libpqdll.pdb" /machine:I386 /out:"$(OUTDIR)\$(OUTFILENAME).dll"\
 /implib:"$(OUTDIR)\$(OUTFILENAME)dll.lib"  /def:$(OUTFILENAME)dll.def
LINK32_OBJS= \
	"$(OUTDIR)\$(OUTFILENAME).lib" \
	"$(OUTDIR)\libpq.res"


# @<< is a Response file, http://www.opussoftware.com/tutorial/TutMakefile.htm

"$(OUTDIR)\$(OUTFILENAME).lib" : "$(OUTDIR)" $(DEF_FILE) $(LIB32_OBJS)
	$(LIB32) @<<
	$(LIB32_FLAGS) $(DEF_FLAGS) $(LIB32_OBJS)
<<

"$(INTDIR)\libpq.res" : "$(INTDIR)" libpq.rc
	$(RSC) $(RSC_PROJ) libpq.rc


"$(OUTDIR)\$(OUTFILENAME).dll" : "$(OUTDIR)" "$(INTDIR)\libpq.res"
	$(LINK32) @<<
	$(LINK32_FLAGS) $(LINK32_OBJS)
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

.c{$(CPP_OBJS)}.obj:
	$(CPP) $(CPP_PROJ) $<

.c.obj:
	$(CPP) $(CPP_PROJ) $<
