#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/nodes
#
# IDENTIFICATION
#    src/backend/nodes/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/nodes
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	bitmapset.o \
	copyfuncs.o \
	equalfuncs.o \
	extensible.o \
	list.o \
	makefuncs.o \
	nodeFuncs.o \
	nodes.o \
	outfuncs.o \
	params.o \
	print.o \
	read.o \
	readfuncs.o \
	tidbitmap.o \
	value.o

include $(top_srcdir)/src/backend/common.mk

node_headers = \
	nodes/nodes.h \
	nodes/execnodes.h \
	nodes/plannodes.h \
	nodes/primnodes.h \
	nodes/pathnodes.h \
	nodes/extensible.h \
	nodes/parsenodes.h \
	nodes/replnodes.h \
	nodes/value.h \
	commands/trigger.h \
	commands/event_trigger.h \
	foreign/fdwapi.h \
	access/amapi.h \
	access/tableam.h \
	access/tsmapi.h \
	utils/rel.h \
	nodes/supportnodes.h \
	executor/tuptable.h \
	nodes/lockoptions.h \
	access/sdir.h

# see also catalog/Makefile for an explanation of these make rules

all: distprep generated-header-symlinks

distprep: node-support-stamp

.PHONY: generated-header-symlinks

generated-header-symlinks: $(top_builddir)/src/include/nodes/header-stamp

# node-support-stamp records the last time we ran gen_node_support.pl.
# We don't rely on the timestamps of the individual output files,
# because the Perl script won't update them if they didn't change (to
# avoid unnecessary recompiles).
node-support-stamp: gen_node_support.pl $(addprefix $(top_srcdir)/src/include/,$(node_headers))
	$(PERL) $^
	touch $@

# These generated headers must be symlinked into builddir/src/include/,
# using absolute links for the reasons explained in src/backend/Makefile.
# We use header-stamp to record that we've done this because the symlinks
# themselves may appear older than node-support-stamp.
$(top_builddir)/src/include/nodes/header-stamp: node-support-stamp
	prereqdir=`cd '$(dir $<)' >/dev/null && pwd` && \
	cd '$(dir $@)' && for file in nodetags.h; do \
	  rm -f $$file && $(LN_S) "$$prereqdir/$$file" . ; \
	done
	touch $@

copyfuncs.o: copyfuncs.c copyfuncs.funcs.c copyfuncs.switch.c | node-support-stamp
equalfuncs.o: equalfuncs.c equalfuncs.funcs.c equalfuncs.switch.c | node-support-stamp
outfuncs.o: outfuncs.c outfuncs.funcs.c outfuncs.switch.c | node-support-stamp
readfuncs.o:  readfuncs.c readfuncs.funcs.c readfuncs.switch.c | node-support-stamp

maintainer-clean: clean
	rm -f node-support-stamp $(addsuffix funcs.funcs.c,copy equal out read) $(addsuffix funcs.switch.c,copy equal out read) nodetags.h
