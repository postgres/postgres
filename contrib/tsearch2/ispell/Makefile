subdir = contrib/tsearch2/ispell
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS := -I$(srcdir) -I$(srcdir)/.. $(CPPFLAGS)

SUBOBJS = spell.o 

all: SUBSYS.o

SUBSYS.o: $(SUBOBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

EXTRA_CLEAN = SUBSYS.o $(SUBOBJS)

include $(top_srcdir)/contrib/contrib-global.mk
# DO NOT DELETE


