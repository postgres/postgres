subdir = src/interfaces/ecpg/test/compat_informix
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress

# special informix compatibility switches
ECPG += -C INFORMIX
ECPG_NOIND  = $(ECPG) -r no_indicator

override LDFLAGS := -L../../compatlib $(LDFLAGS)
override LIBS := -lecpg_compat $(LIBS)

TESTS = test_informix test_informix.c \
        test_informix2 test_informix2.c \
        dec_test dec_test.c \
        rfmtdate rfmtdate.c \
        rfmtlong rfmtlong.c \
        rnull rnull.c \
        sqlda sqlda.c \
        charfuncs charfuncs.c

all: $(TESTS)

test_informix.c: test_informix.pgc ../regression.h
	$(ECPG) -o $@ -I$(srcdir) $<

test_informix2.c: test_informix2.pgc ../regression.h
	$(ECPG) -o $@ -I$(srcdir) $<

sqlda.c: sqlda.pgc ../regression.h
	$(ECPG) -o $@ -I$(srcdir) $<

dec_test.c: dec_test.pgc ../regression.h
	$(ECPG) -o $@ -I$(srcdir) $<

rfmtdate.c: rfmtdate.pgc ../regression.h
	$(ECPG) -o $@ -I$(srcdir) $<

rfmtlong.c: rfmtlong.pgc ../regression.h
	$(ECPG) -o $@ -I$(srcdir) $<

rnull.c: rnull.pgc ../regression.h
	$(ECPG_NOIND) -o $@ -I$(srcdir) $<

charfuncs.c: charfuncs.pgc ../regression.h
	$(ECPG) -o $@ -I$(srcdir) $<
