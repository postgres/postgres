#-------------------------------------------------------------------------
#
# Makefile for the bootstrap module
#
# $Header: /cvsroot/pgsql/src/backend/bootstrap/Makefile,v 1.31 2002/11/01 22:52:33 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/bootstrap
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)

# qnx4's wlink currently crashes with bootstrap.o
ifneq ($(PORTNAME), qnx4)
OBJS= bootparse.o bootstrap.o 
else
OBJS= bootparse.o
endif


# make sure bootstrap.o is built even on qnx4
all: SUBSYS.o bootstrap.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^


bootstrap.o bootparse.o: $(srcdir)/bootstrap_tokens.h

# bootscanner is compiled as part of bootparse
bootparse.o: $(srcdir)/bootscanner.c

# `sed' rules to remove conflicts between bootstrap scanner and parser
# and the SQL scanner and parser. For correctness' sake the rules that
# use this must depend on this Makefile.
define sed-magic
sed -e 's/^yy/Int_yy/g' \
    -e 's/\([^a-zA-Z0-9_]\)yy/\1Int_yy/g'
endef


$(srcdir)/bootparse.c: $(srcdir)/bootstrap_tokens.h ;

$(srcdir)/bootstrap_tokens.h: bootparse.y Makefile
ifdef YACC
	$(YACC) -d $(YFLAGS) $<
	$(sed-magic) < y.tab.c > $(srcdir)/bootparse.c
	$(sed-magic) < y.tab.h > $(srcdir)/bootstrap_tokens.h
	rm -f y.tab.c y.tab.h
else
	@$(missing) bison $< $@
endif

$(srcdir)/bootscanner.c: bootscanner.l Makefile
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) $<
	$(sed-magic) lex.yy.c > $@
	rm -f lex.yy.c
else
	@$(missing) flex $< $@
endif

clean:
	rm -f SUBSYS.o $(OBJS) bootstrap.o
# And the garbage that might have been left behind by partial build:
	@rm -f y.tab.h y.tab.c y.output lex.yy.c


# This is unusual:  We actually have to build some of the parts before
# we know what the header file dependencies are.  
dep depend: bootparse.c bootscanner.c bootstrap_tokens.h
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
