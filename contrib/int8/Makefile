#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for Postgres 64-bit integer extensions
#
# Thomas G. Lockhart <Thomas.Lockhart@jpl.nasa.gov>
#
# This is a first attempt at 64-bit arithmetic for Postgres.
# It takes advantage of "long long int" support in GNU C on 32-bit machines.
# The modules are built and installed as user-defined types,
#  so destination directories are pointing away from the standard
#  Postgres areas. You will need to modify the paths to fit your machine.
#
# On my Linux box, I had to find an extra library for the division function (?).
# For Alpha boxes, both the DEC and GNU compilers should need "long int" only.
#
#-------------------------------------------------------------------------

ifndef PGDIR
PGDIR= ../..
endif

SRCDIR= $(PGDIR)/src

include $(SRCDIR)/Makefile.global

# Comment out this re-declaration of LIBDIR
#  if you are installing as the postgres superuser
#   into a specific database or into template1.
#LIBDIR= /home/tgl/lib

CFLAGS+= -I$(PGDIR)/include -I$(PGDIR)/src/include -I$(LIBPQDIR)

# This extra library is for the 64-bit division routine on my Linux box
# and probably will need to be commented-out for most other platforms.
#CLIBS+= /usr/lib/gcc-lib/i486-linux/2.7.2/libgcc.a

TARGETS= int8.sql int8$(DLSUFFIX)

all:	$(TARGETS)

int8$(DLSUFFIX):	int8.o
	$(CC) -shared -o int8$(DLSUFFIX) int8.o $(CLIBS)

install:
	$(MAKE) all
	cp -p int8$(DLSUFFIX) $(LIBDIR)/modules

%.sql: %.source
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $$USER.'; exit 1; fi; \
	rm -f $@; \
	C=`pwd`; \
	O=$C; \
	if [ -d ${LIBDIR}/contrib ]; then O=${LIBDIR}/contrib; fi; \
	sed -e "s:_CWD_:$$C:g" \
	    -e "s:_OBJWD_:$$O:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	    -e "s/_USER_/$$USER/g" < $< > $@

clean: 
	rm -f $(TARGETS) int8.o

