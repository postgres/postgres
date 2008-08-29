#-------------------------------------------------------------------------
#
# Makefile for the bootstrap module
#
# $PostgreSQL: pgsql/src/backend/bootstrap/Makefile,v 1.37 2008/08/29 13:02:32 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/bootstrap
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)

OBJS= bootparse.o bootstrap.o 

include $(top_srcdir)/src/backend/common.mk


# bootscanner is compiled as part of bootparse
bootparse.o: $(srcdir)/bootscanner.c

$(srcdir)/bootparse.c: bootparse.y
ifdef BISON
	$(BISON) $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

$(srcdir)/bootscanner.c: bootscanner.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif


# bootparse.c and bootscanner.c are in the distribution tarball, so
# they are not cleaned here.
