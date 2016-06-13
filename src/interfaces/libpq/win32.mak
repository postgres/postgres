# Makefile for Microsoft Visual C++ 7.1-8.0

# Will build a static library libpq(d).lib
#        and a dynamic library libpq(d).dll with import library libpq(d)dll.lib
# USE_OPENSSL=1 will compile with OpenSSL
# USE_KFW=1 will compile with kfw(kerberos for Windows)
# DEBUG=1 compiles with debugging symbols
# ENABLE_THREAD_SAFETY=1 compiles with threading enabled

ENABLE_THREAD_SAFETY=1

# CPU="i386" or CPU environment of nmake.exe (AMD64 or IA64)

!IF ("$(CPU)" == "")||("$(CPU)" == "i386")
CPU=i386
!MESSAGE Building the Win32 static library...
!MESSAGE
!ELSEIF ("$(CPU)" == "IA64")||("$(CPU)" == "AMD64")
ADD_DEFINES=/Wp64 /GS
ADD_SECLIB=bufferoverflowU.lib
!MESSAGE Building the Win64 static library...
!MESSAGE
!ELSE
!MESSAGE Please check a CPU=$(CPU) ?
!MESSAGE CPU=i386 or AMD64 or IA64
!ERROR Make aborted.
!ENDIF

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

!IF "$(SSL_INC)" == ""
SSL_INC=C:\OpenSSL\include
!MESSAGE Using default OpenSSL Include directory: $(SSL_INC)
!ENDIF

!IF "$(SSL_LIB_PATH)" == ""
SSL_LIB_PATH=C:\OpenSSL\lib\VC
!MESSAGE Using default OpenSSL Library directory: $(SSL_LIB_PATH)
!ENDIF

!IF "$(KFW_INC)" == ""
KFW_INC=C:\kfw-2.6.5\inc
!MESSAGE Using default Kerberos Include directory: $(KFW_INC)
!ENDIF

!IF "$(KFW_LIB_PATH)" == ""
KFW_LIB_PATH=C:\kfw-2.6.5\lib\$(CPU)
!MESSAGE Using default Kerberos Library directory: $(KFW_LIB_PATH)
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
	-@erase "$(INTDIR)\pqsignal.obj"
	-@erase "$(INTDIR)\thread.obj"
	-@erase "$(INTDIR)\inet_aton.obj"
	-@erase "$(INTDIR)\crypt.obj"
	-@erase "$(INTDIR)\noblock.obj"
	-@erase "$(INTDIR)\chklocale.obj"
	-@erase "$(INTDIR)\inet_net_ntop.obj"
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
	-@erase "$(INTDIR)\system.obj"
	-@erase "$(INTDIR)\win32error.obj"
	-@erase "$(INTDIR)\win32setlocale.obj"
	-@erase "$(OUTDIR)\$(OUTFILENAME).lib"
	-@erase "$(OUTDIR)\$(OUTFILENAME)dll.lib"
	-@erase "$(OUTDIR)\libpq.res"
	-@erase "$(OUTDIR)\$(OUTFILENAME).dll"
	-@erase "$(OUTDIR)\$(OUTFILENAME)dll.exp"
	-@erase "$(OUTDIR)\$(OUTFILENAME).dll.manifest"
	-@erase "$(OUTDIR)\*.idb"
	-@erase pg_config_paths.h"
!IFDEF USE_OPENSSL
	-@erase "$(INTDIR)\fe-secure-openssl.obj"
!ENDIF


LIB32=link.exe -lib
LIB32_FLAGS=$(LOPT) /nologo /out:"$(OUTDIR)\$(OUTFILENAME).lib"
LIB32_OBJS= \
	"$(INTDIR)\win32.obj" \
	"$(INTDIR)\getaddrinfo.obj" \
	"$(INTDIR)\pgstrcasecmp.obj" \
	"$(INTDIR)\pqsignal.obj" \
	"$(INTDIR)\thread.obj" \
	"$(INTDIR)\inet_aton.obj" \
	"$(INTDIR)\crypt.obj" \
	"$(INTDIR)\noblock.obj" \
	"$(INTDIR)\chklocale.obj" \
	"$(INTDIR)\inet_net_ntop.obj" \
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
	"$(INTDIR)\wchar.obj" \
	"$(INTDIR)\encnames.obj" \
	"$(INTDIR)\snprintf.obj" \
	"$(INTDIR)\strlcpy.obj" \
	"$(INTDIR)\dirent.obj" \
	"$(INTDIR)\dirmod.obj" \
	"$(INTDIR)\pgsleep.obj" \
	"$(INTDIR)\open.obj" \
	"$(INTDIR)\system.obj" \
	"$(INTDIR)\win32error.obj" \
	"$(INTDIR)\win32setlocale.obj" \
	"$(INTDIR)\pthread-win32.obj"

!IFDEF USE_OPENSSL
LIB32_OBJS=$(LIB32_OBJS) "$(INTDIR)\fe-secure-openssl.obj"
!ENDIF


config: ..\..\include\pg_config.h ..\..\include\pg_config_ext.h pg_config_paths.h  ..\..\include\pg_config_os.h

..\..\include\pg_config.h: ..\..\include\pg_config.h.win32
	copy ..\..\include\pg_config.h.win32 ..\..\include\pg_config.h

..\..\include\pg_config_ext.h: ..\..\include\pg_config_ext.h.win32
	copy ..\..\include\pg_config_ext.h.win32 ..\..\include\pg_config_ext.h

..\..\include\pg_config_os.h:
	copy ..\..\include\port\win32.h ..\..\include\pg_config_os.h

pg_config_paths.h: win32.mak
	echo #define SYSCONFDIR "" > pg_config_paths.h

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo /W3 /EHsc $(OPT) /I "..\..\include" /I "..\..\include\port\win32" /I "..\..\include\port\win32_msvc" /I "..\..\port" /I. /I "$(SSL_INC)" \
 /D "FRONTEND" $(DEBUGDEF) \
 /D "WIN32" /D "_WINDOWS" /Fp"$(INTDIR)\libpq.pch" \
 /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c  \
 /D "_CRT_SECURE_NO_DEPRECATE" $(ADD_DEFINES)

!IFDEF USE_OPENSSL
CPP_PROJ=$(CPP_PROJ) /D USE_OPENSSL
SSL_LIBS=ssleay32.lib libeay32.lib gdi32.lib
!ENDIF

!IFDEF USE_KFW
CPP_PROJ=$(CPP_PROJ) /D KRB5
KFW_LIBS=krb5_32.lib comerr32.lib gssapi32.lib
!ENDIF

!IFDEF ENABLE_THREAD_SAFETY
CPP_PROJ=$(CPP_PROJ) /D ENABLE_THREAD_SAFETY
!ENDIF

CPP_SBRS=.

RSC_PROJ=/l 0x409 /fo"$(INTDIR)\libpq.res"

LINK32=link.exe
LINK32_FLAGS=kernel32.lib user32.lib advapi32.lib shell32.lib ws2_32.lib secur32.lib $(SSL_LIBS)  $(KFW_LIB) $(ADD_SECLIB) \
 /nologo /subsystem:windows /dll $(LOPT) /incremental:no \
 /pdb:"$(OUTDIR)\libpqdll.pdb" /machine:$(CPU) \
 /out:"$(OUTDIR)\$(OUTFILENAME).dll"\
 /implib:"$(OUTDIR)\$(OUTFILENAME)dll.lib"  \
 /libpath:"$(SSL_LIB_PATH)" /libpath:"$(KFW_LIB_PATH)" \
 /def:$(OUTFILENAME)dll.def
LINK32_OBJS= \
	"$(OUTDIR)\$(OUTFILENAME).lib" \
	"$(OUTDIR)\libpq.res"

# @<< is a Response file, http://www.opussoftware.com/tutorial/TutMakefile.htm

"$(OUTDIR)\$(OUTFILENAME).lib" : "$(OUTDIR)" $(DEF_FILE) $(LIB32_OBJS)
	$(LIB32) @<<
	$(LIB32_FLAGS) $(DEF_FLAGS) $(LIB32_OBJS)
<<

"$(INTDIR)\libpq.res" : "$(INTDIR)" libpq-dist.rc
	$(RSC) $(RSC_PROJ) libpq-dist.rc


"$(OUTDIR)\$(OUTFILENAME).dll" : "$(OUTDIR)" "$(INTDIR)\libpq.res"
	$(LINK32) @<<
	$(LINK32_FLAGS) $(LINK32_OBJS)
<<
# Inclusion of manifest
!IF "$(_NMAKE_VER)" != "6.00.8168.0" && "$(_NMAKE_VER)" != "7.00.9466"
!IF "$(_NMAKE_VER)" != "6.00.9782.0" && "$(_NMAKE_VER)" != "7.10.3077"
        mt -manifest $(OUTDIR)\$(OUTFILENAME).dll.manifest -outputresource:$(OUTDIR)\$(OUTFILENAME).dll;2
!ENDIF
!ENDIF

"$(INTDIR)\getaddrinfo.obj" : ..\..\port\getaddrinfo.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\getaddrinfo.c
<<

"$(INTDIR)\pgstrcasecmp.obj" : ..\..\port\pgstrcasecmp.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\pgstrcasecmp.c
<<

"$(INTDIR)\pqsignal.obj" : ..\..\port\pqsignal.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\pqsignal.c
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

"$(INTDIR)\chklocale.obj" : ..\..\port\chklocale.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\chklocale.c
<<

"$(INTDIR)\inet_net_ntop.obj" : ..\..\port\inet_net_ntop.c
	$(CPP) @<<
	$(CPP_PROJ) ..\..\port\inet_net_ntop.c
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

"$(INTDIR)\system.obj" : ..\..\port\system.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\port\system.c
<<

"$(INTDIR)\win32error.obj" : ..\..\port\win32error.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\port\win32error.c
<<

"$(INTDIR)\win32setlocale.obj" : ..\..\port\win32setlocale.c
	$(CPP) @<<
	$(CPP_PROJ) /I"." ..\..\port\win32setlocale.c
<<

.c{$(CPP_OBJS)}.obj:
	$(CPP) $(CPP_PROJ) $<

.c.obj:
	$(CPP) $(CPP_PROJ) $<
