
# cflags.  pick your favorite
#
CC=gcc
CFLAGS=-g -O0 -Wall -Wmissing-declarations -fPIC

# build info for python, alter as needed
#

# python headers
#
#INCPYTHON=/usr/include/python1.5
INCPYTHON=/usr/include/python2.0

# python shared library
#
#LIBPYTHON=python1.5
LIBPYTHON=python2.0

# if python is someplace odd
#
LIBPYTHONPATH=/usr/lib

# python 2 seems to want libdb
# various db libs are still messed on my system
#
#LIBPYTHONEXTRA=/usr/lib/libdb2.so.2.7.7
#LIBPYTHONEXTRA=-ldb2

LDPYTHON=-L$(LIBPYTHONPATH) -l$(LIBPYTHON) $(LIBPYTHONEXTRA)

# build info for postgres
#

# postgres headers.  the installed include directory doesn't work for me
#
#INCPOSTGRES=/usr/include/postgres
INCPOSTGRES=/home/andrew/builds/postgresql/src/include

# hopefully you won't need this utter crap...
# but if you can't patch the appropriate dynloader file, try this.  you
# may have to add other modules.
#
#DLDIR=/usr/lib/python1.5/lib-dynload
#DLHACK=$(DLDIR)/arraymodule.so $(DLDIR)/timemodule.so $(DLDIR)/cmathmodule.so $(DLDIR)/errnomodule.so $(DLDIR)/mathmodule.so $(DLDIR)/md5module.so $(DLDIR)/operator.so
# $(DLDIR)/shamodule.so

# shouldn't need to alter anything below here
#
INCLUDES=-I$(INCPYTHON) -I$(INCPOSTGRES) -I./

# dynamic linker flags.  
#
#LDFLAGS=--shared -Wl,-Bshareable -Wl,-E -Wl,-soname,$@
LDFLAGS=--shared -Wl,-E -Wl,-soname,$@

.PHONY: clean

all: plpython.so 

plpython.o: plpython.c plpython.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

plpython.so: plpython.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDPYTHON) $(DLHACK) -ldl -lpthread -lm

clean:
	rm -f plpython.so *.o

