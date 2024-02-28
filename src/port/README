src/port/README

libpgport
=========

libpgport must have special behavior.  It supplies functions to both
libraries and applications.  However, there are two complexities:

1)  Libraries need to use object files that are compiled with exactly
the same flags as the library.  libpgport might not use the same flags,
so it is necessary to recompile the object files for individual
libraries.  This is done by removing -lpgport from the link line:

        # Need to recompile any libpgport object files
        LIBS := $(filter-out -lpgport, $(LIBS))

and adding infrastructure to recompile the object files:

        OBJS= execute.o typename.o descriptor.o data.o error.o prepare.o memory.o \
                connect.o misc.o path.o exec.o \
                $(filter strlcat.o, $(LIBOBJS))

The problem is that there is no testing of which object files need to be
added, but missing functions usually show up when linking user
applications.

2) For applications, we use -lpgport before -lpq, so the static files
from libpgport are linked first.  This avoids having applications
dependent on symbols that are _used_ by libpq, but not intended to be
exported by libpq.  libpq's libpgport usage changes over time, so such a
dependency is a problem.  Windows, Linux, and macOS use an export
list to control the symbols exported by libpq.
