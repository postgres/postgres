#-------------------------------------------------------------------------
#
# Makefile for parser
#
# $PostgreSQL: pgsql/src/backend/parser/Makefile,v 1.44 2006/05/27 17:38:45 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/parser
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)

OBJS= analyze.o gram.o keywords.o parser.o parse_agg.o parse_clause.o \
      parse_expr.o parse_func.o parse_node.o parse_oper.o parse_relation.o \
      parse_type.o parse_coerce.o parse_target.o scansup.o

FLEXFLAGS = -CF


all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^


# scan is compiled as part of gram
gram.o: $(srcdir)/scan.c


# There is no correct way to write a rule that generates two files.
# Rules with two targets don't have that meaning, they are merely
# shorthand for two otherwise separate rules.  To be safe for parallel
# make, we must chain the dependencies like this.  The semicolon is
# important, otherwise make will choose the built-in rule for
# gram.y=>gram.c.

$(srcdir)/gram.c: $(srcdir)/parse.h ;

$(srcdir)/parse.h: gram.y
ifdef YACC
	$(YACC) -d $(YFLAGS) $<
	mv -f y.tab.c $(srcdir)/gram.c
	mv -f y.tab.h $(srcdir)/parse.h
else
	@$(missing) bison $< $@
endif


$(srcdir)/scan.c: scan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif


# Force these dependencies to be known even without dependency info built:
gram.o keywords.o parser.o: $(srcdir)/parse.h


# gram.c, parse.h, and scan.c are in the distribution tarball, so they
# are not cleaned here.
clean: 
	rm -f SUBSYS.o $(OBJS)
# And the garbage that might have been left behind by partial build:
	@rm -f y.tab.h y.tab.c y.output lex.yy.c
