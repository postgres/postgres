# Makefile for Borland C++ 5.5

# Borland C++ base install directory goes here
# BCB=c:\Borland\Bcc55

!IF "$(BCB)" == ""
!MESSAGE You must edit bcc32.mak and define BCB at the top
!ERROR missing BCB
!ENDIF

!IF "$(__NMAKE__)" == ""
!MESSAGE You must use the -N compatibility flag, e.g. make -N -f bcc32.make
!ERROR missing -N
!ENDIF

!MESSAGE Building PSQL.EXE ...
!MESSAGE
!IF "$(CFG)" == ""
CFG=Release
!MESSAGE No configuration specified. Defaulting to Release with STATIC libraries.
!MESSAGE To use dynamic link libraries add -DDLL_LIBS to make command line.
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
!MESSAGE "Release" (Win32 Release EXE)
!MESSAGE "Debug" (Win32 Debug EXE)
!MESSAGE
!ERROR An invalid configuration was specified.
!ENDIF

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

CPP=bcc32.exe
PERL=perl.exe
FLEX=flex.exe

!IF "$(CFG)" == "Debug"
DEBUG=1
OUTDIR=.\Debug
INTDIR=.\Debug
!else
OUTDIR=.\Release
INTDIR=.\Release
!endif
REFDOCDIR=../../../doc/src/sgml/ref

CPP_PROJ = -I$(BCB)\include;..\..\include;..\..\interfaces\libpq;..\..\include\port\win32;..\..\include\port\win32_msvc;..\pg_dump;..\..\backend \
           -c -D$(USERDEFINES) -DFRONTEND -n"$(INTDIR)" -tWM -tWC -q -5 -a8 -pc -X -w-use \
	   -w-par -w-pia -w-csu -w-aus -w-ccc

!IFDEF DEBUG
CPP_PROJ  	= $(CPP_PROJ) -Od -r- -k -v -y -vi- -D_DEBUG
LIBPG_DIR 	= Debug
!ELSE
CPP_PROJ	= $(CPP_PROJ) -O -Oi -OS -DNDEBUG
LIBPG_DIR 	= Release
!ENDIF

!IFDEF DLL_LIBS
CPP_PROJ	= $(CPP_PROJ) -D_RTLDLL
LIBRARIES	= cw32mti.lib ..\..\interfaces\libpq\$(LIBPG_DIR)\blibpqdll.lib
!ELSE
CPP_PROJ	= $(CPP_PROJ) -DBCC32_STATIC
LIBRARIES	= cw32mt.lib ..\..\interfaces\libpq\$(LIBPG_DIR)\blibpq.lib
!ENDIF

.path.obj = $(INTDIR)

USERDEFINES = WIN32;_CONSOLE;_MBCS

ALL : sql_help.h psqlscan.c "..\..\port\pg_config_paths.h" "$(OUTDIR)\psql.exe"

CLEAN :
	-@erase "$(INTDIR)\command.obj"
	-@erase "$(INTDIR)\common.obj"
	-@erase "$(INTDIR)\copy.obj"
	-@erase "$(INTDIR)\describe.obj"
	-@erase "$(INTDIR)\help.obj"
	-@erase "$(INTDIR)\input.obj"
	-@erase "$(INTDIR)\large_obj.obj"
	-@erase "$(INTDIR)\mainloop.obj"
	-@erase "$(INTDIR)\mbprint.obj"
	-@erase "$(INTDIR)\print.obj"
	-@erase "$(INTDIR)\prompt.obj"
	-@erase "$(INTDIR)\psqlscan.obj"
	-@erase "$(INTDIR)\startup.obj"
	-@erase "$(INTDIR)\stringutils.obj"
	-@erase "$(INTDIR)\tab-complete.obj"
	-@erase "$(INTDIR)\variables.obj"
	-@erase "$(INTDIR)\exec.obj"
	-@erase "$(INTDIR)\getopt.obj"
	-@erase "$(INTDIR)\getopt_long.obj"
	-@erase "$(INTDIR)\snprintf.obj"
	-@erase "$(INTDIR)\path.obj"
	-@erase "$(INTDIR)\strlcpy.obj"
	-@erase "$(INTDIR)\pgstrcasecmp.obj"
	-@erase "$(INTDIR)\sprompt.obj"
	-@erase "$(INTDIR)\dumputils.obj"
	-@erase "$(INTDIR)\keywords.obj"
	-@erase "$(INTDIR)\psql.ilc"
	-@erase "$(INTDIR)\psql.ild"
	-@erase "$(INTDIR)\psql.tds"
	-@erase "$(INTDIR)\psql.ils"
	-@erase "$(INTDIR)\psql.ilf"
	-@erase "$(OUTDIR)\psql.exe"
	-@erase "$(INTDIR)\..\..\port\pg_config_paths.h"

LINK32=ilink32.exe
LINK32_FLAGS=-L$(BCB)\lib;.\$(LIBPG_DIR) -x -v
LINK32_OBJS= \
	"$(INTDIR)\command.obj" \
	"$(INTDIR)\common.obj" \
	"$(INTDIR)\copy.obj" \
	"$(INTDIR)\describe.obj" \
	"$(INTDIR)\help.obj" \
	"$(INTDIR)\input.obj" \
	"$(INTDIR)\large_obj.obj" \
	"$(INTDIR)\mainloop.obj" \
	"$(INTDIR)\mbprint.obj" \
	"$(INTDIR)\print.obj" \
	"$(INTDIR)\prompt.obj" \
	"$(INTDIR)\psqlscan.obj" \
	"$(INTDIR)\startup.obj" \
	"$(INTDIR)\stringutils.obj" \
	"$(INTDIR)\tab-complete.obj" \
	"$(INTDIR)\variables.obj" \
	"$(INTDIR)\exec.obj" \
	"$(INTDIR)\getopt.obj" \
	"$(INTDIR)\getopt_long.obj" \
	"$(INTDIR)\snprintf.obj" \
	"$(INTDIR)\path.obj" \
	"$(INTDIR)\strlcpy.obj" \
	"$(INTDIR)\pgstrcasecmp.obj" \
	"$(INTDIR)\sprompt.obj" \
	"$(INTDIR)\dumputils.obj" \
	"$(INTDIR)\keywords.obj" 

!IFDEF DEBUG
LINK32_OBJS	= $(LINK32_OBJS) "..\..\interfaces\libpq\Debug\blibpqddll.lib"
!ELSE
LINK32_OBJS	= $(LINK32_OBJS) "..\..\interfaces\libpq\Release\blibpqdll.lib"
!ENDIF

# Have to use \# so # isn't treated as a comment, but MSVC doesn't like this
"..\..\port\pg_config_paths.h": bcc32.mak
	echo \#define PGBINDIR "" >$@
	echo \#define PGSHAREDIR "" >>$@
	echo \#define SYSCONFDIR "" >>$@
	echo \#define INCLUDEDIR "" >>$@
	echo \#define PKGINCLUDEDIR "" >>$@
	echo \#define INCLUDEDIRSERVER "" >>$@
	echo \#define LIBDIR "" >>$@
	echo \#define PKGLIBDIR "" >>$@
	echo \#define LOCALEDIR "" >>$@
	echo \#define DOCDIR "" >>$@
	echo \#define MANDIR "" >>$@

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

"$(OUTDIR)\psql.exe" : "$(OUTDIR)" $(LINK32_OBJS)
	$(LINK32) @&&!
	$(LINK32_FLAGS) +
	c0x32.obj $(LINK32_OBJS), +
	$@,, +
	import32.lib $(LIBRARIES),,
!

"$(INTDIR)\exec.obj" : ..\..\port\exec.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\exec.c
<<

"$(INTDIR)\getopt.obj" : "$(INTDIR)" ..\..\port\getopt.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\getopt.c
<<

"$(INTDIR)\getopt_long.obj" : "$(INTDIR)" ..\..\port\getopt_long.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\getopt_long.c
<<

"$(INTDIR)\snprintf.obj" : "$(INTDIR)" ..\..\port\snprintf.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\snprintf.c
<<

"$(INTDIR)\path.obj" : "$(INTDIR)" ..\..\port\path.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\path.c
<<

"$(INTDIR)\strlcpy.obj" : "$(INTDIR)" ..\..\port\strlcpy.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\strlcpy.c
<<

"$(INTDIR)\pgstrcasecmp.obj" : ..\..\port\pgstrcasecmp.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\pgstrcasecmp.c
<<

"$(INTDIR)\sprompt.obj" : "$(INTDIR)" ..\..\port\sprompt.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\port\sprompt.c
<<

"$(INTDIR)\dumputils.obj" : "$(INTDIR)" ..\pg_dump\dumputils.c
    $(CPP) @<<
    $(CPP_PROJ) ..\pg_dump\dumputils.c
<<

"$(INTDIR)\keywords.obj" : "$(INTDIR)" ..\..\backend\parser\keywords.c
    $(CPP) @<<
    $(CPP_PROJ) ..\..\backend\parser\keywords.c
<<

"sql_help.h": create_help.pl 
       $(PERL) create_help.pl $(REFDOCDIR) $@

psqlscan.c : psqlscan.l
	$(FLEX) -Cfe -opsqlscan.c psqlscan.l

.c.obj:
	$(CPP) -o"$(INTDIR)\$&" $(CPP_PROJ) $<

