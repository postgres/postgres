#-------------------------------------------------------------------------
#
# Makefile for parser
#
# $Header: /cvsroot/pgsql/src/backend/parser/Makefile,v 1.32 2001/02/10 22:31:42 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/parser
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(GCC), yes)
override CFLAGS+= -Wno-error
endif

OBJS= analyze.o gram.o keywords.o parser.o parse_agg.o parse_clause.o \
      parse_expr.o parse_func.o parse_node.o parse_oper.o parse_relation.o \
      parse_type.o parse_coerce.o parse_target.o scan.o scansup.o


all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

$(srcdir)/gram.c $(srcdir)/parse.h: gram.y
ifdef YACC
	$(YACC) -d $(YFLAGS) $<
	mv y.tab.c $(srcdir)/gram.c
	mv y.tab.h $(srcdir)/parse.h
else
	@$(missing) bison $< $@
endif

$(srcdir)/scan.c: scan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -Pbase_yy -o'$@' $<
else
	@$(missing) flex $< $@
endif


analyze.o keywords.o parse_clause.o parse_expr.o scan.o: $(srcdir)/parse.h


# gram.c, parse.h, and scan.c are in the distribution tarball, so they
# are not cleaned here.
clean: 
	rm -f SUBSYS.o $(OBJS)
# And the garbage that might have been left behind by partial build:
	@rm -f y.tab.c y.tab.h lex.yy.c


# This is unusual:  We actually have to build some of the parts before
# we know what the header file dependencies are.  
dep depend: gram.c scan.c
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
