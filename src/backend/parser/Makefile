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

OBJS = \
	analyze.o \
	gram.o \
	parse_agg.o \
	parse_clause.o \
	parse_coerce.o \
	parse_collate.o \
	parse_cte.o \
	parse_enr.o \
	parse_expr.o \
	parse_func.o \
	parse_jsontable.o \
	parse_merge.o \
	parse_node.o \
	parse_oper.o \
	parse_param.o \
	parse_relation.o \
	parse_target.o \
	parse_type.o \
	parse_utilcmd.o \
	parser.o \
	scan.o \
	scansup.o

include $(top_srcdir)/src/backend/common.mk


# There is no correct way to write a rule that generates two files.
# Rules with two targets don't have that meaning, they are merely
# shorthand for two otherwise separate rules.  If we have an action
# that in fact generates two or more files, we must choose one of them
# as primary and show it as the action's output, then make all of the
# other output files dependent on the primary, like this.  Furthermore,
# the "touch" action is essential, because it ensures that gram.h is
# marked as newer than (or at least no older than) gram.c.  Without that,
# make is likely to try to rebuild gram.h in subsequent runs, which causes
# failures in VPATH builds from tarballs.

gram.h: gram.c
	touch $@

gram.c: BISONFLAGS += -d
gram.c: BISON_CHECK_CMD = $(PERL) $(srcdir)/check_keywords.pl $< $(top_srcdir)/src/include/parser/kwlist.h


scan.c: FLEXFLAGS = -CF -p -p
scan.c: FLEX_NO_BACKUP=yes
scan.c: FLEX_FIX_WARNING=yes


# Force these dependencies to be known even without dependency info built:
gram.o scan.o parser.o: gram.h

clean:
	rm -f gram.c \
	      gram.h \
	      scan.c
	rm -f lex.backup
