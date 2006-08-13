subdir = src/interfaces/ecpg/test/compat_informix
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress

# special informix compatiblity switches
ECPG += -C INFORMIX
ECPG_NOIND  = $(ECPG) -r no_indicator
override LDFLAGS += -L../../compatlib
override LIBS += $(LIBS) -lecpg_compat

TESTS = test_informix test_informix.c \
        test_informix2 test_informix2.c

all: $(TESTS)

test_informix.c: test_informix.pgc ../regression.h
	$(ECPG) -o $@ -I$(srcdir) $<

test_informix2.c: test_informix2.pgc ../regression.h
	$(ECPG_NOIND) -o $@ -I$(srcdir) $<

