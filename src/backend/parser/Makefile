#-------------------------------------------------------------------------
#
# Makefile for parser
#
# src/backend/parser/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/parser
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS= analyze.o gram.o keywords.o kwlookup.o parser.o \
      parse_agg.o parse_clause.o parse_coerce.o parse_collate.o parse_cte.o \
      parse_expr.o parse_func.o parse_node.o parse_oper.o parse_param.o \
      parse_relation.o parse_target.o parse_type.o parse_utilcmd.o scansup.o

include $(top_srcdir)/src/backend/common.mk


# scan is compiled as part of gram
gram.o: scan.c

# Latest flex causes warnings in this file.
ifeq ($(GCC),yes)
gram.o: CFLAGS += -Wno-error
endif


# There is no correct way to write a rule that generates two files.
# Rules with two targets don't have that meaning, they are merely
# shorthand for two otherwise separate rules.  To be safe for parallel
# make, we must chain the dependencies like this.  The semicolon is
# important, otherwise make will choose the built-in rule for
# gram.y=>gram.c.

gram.h: gram.c ;

gram.c: BISONFLAGS += -d
gram.c: BISON_CHECK_CMD = $(PERL) $(srcdir)/check_keywords.pl $< $(top_srcdir)/src/include/parser/kwlist.h


scan.c: FLEXFLAGS = -CF -p -p
scan.c: FLEX_NO_BACKUP=yes


# Force these dependencies to be known even without dependency info built:
gram.o keywords.o parser.o: gram.h


# gram.c, gram.h, and scan.c are in the distribution tarball, so they
# are not cleaned here.
clean distclean maintainer-clean:
	rm -f lex.backup
