SRCDIR= ../../src

include $(SRCDIR)/Makefile.global

CONTRIBDIR=$(LIBDIR)/contrib

CFLAGS+= $(CFLAGS_SL)

TARGETS= fti$(DLSUFFIX)
CLEANFILES+= $(TARGETS)
CURDIR=`pwd`

all:: $(TARGETS)

%.sql: %.source
	rm -f $@; \
	sed -e "s:_CURRENTDIR_:$(CURDIR):g" \
		-e "s:_DLSUFFIX_:$(DLSUFFIX):g" < $< > $@

clean: 
	rm -f $(TARGETS) *.o

dist:
	tar cf fti.tar README BENCH Makefile fti.c timings.sh
