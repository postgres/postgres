subdir = contrib/tsearch2/snowball
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS := -I$(srcdir) -I$(srcdir)/.. $(CPPFLAGS)
override CFLAGS += $(CFLAGS_SL)

SUBOBJS = english_stem.o api.o russian_stem.o utilities.o

all: SUBSYS.o

SUBSYS.o: $(SUBOBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

EXTRA_CLEAN = SUBSYS.o $(SUBOBJS)

include $(top_srcdir)/contrib/contrib-global.mk
# DO NOT DELETE


