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

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = \
	bitmapset.o \
	copyfuncs.o \
	equalfuncs.o \
	extensible.o \
	list.o \
	makefuncs.o \
	multibitmapset.o \
	nodeFuncs.o \
	outfuncs.o \
	params.o \
	print.o \
	queryjumblefuncs.o \
	read.o \
	readfuncs.o \
	tidbitmap.o \
	value.o

include $(top_srcdir)/src/backend/common.mk

# The principal node-defining headers are listed in their inclusion order,
# to ensure cross-file subtyping works correctly.  The rest can just be
# listed alphabetically.
# Caution: re-ordering this list risks ABI breakage!
node_headers = \
	nodes/nodes.h \
	nodes/primnodes.h \
	nodes/parsenodes.h \
	nodes/pathnodes.h \
	nodes/plannodes.h \
	nodes/execnodes.h \
	access/amapi.h \
	access/sdir.h \
	access/tableam.h \
	access/tsmapi.h \
	commands/event_trigger.h \
	commands/trigger.h \
	executor/tuptable.h \
	foreign/fdwapi.h \
	nodes/bitmapset.h \
	nodes/extensible.h \
	nodes/lockoptions.h \
	nodes/miscnodes.h \
	nodes/replnodes.h \
	nodes/supportnodes.h \
	nodes/value.h \
	utils/rel.h

# see also catalog/Makefile for an explanation of these make rules

all: generated-header-symlinks

.PHONY: generated-header-symlinks

generated-header-symlinks: $(top_builddir)/src/include/nodes/header-stamp

# node-support-stamp records the last time we ran gen_node_support.pl.
# We don't rely on the timestamps of the individual output files,
# because the Perl script won't update them if they didn't change (to
# avoid unnecessary recompiles).
node-support-stamp: gen_node_support.pl $(addprefix $(top_srcdir)/src/include/,$(node_headers))
	$(PERL) $^
	touch $@

# These generated headers must be symlinked into src/include/.
# We use header-stamp to record that we've done this because the symlinks
# themselves may appear older than node-support-stamp.
$(top_builddir)/src/include/nodes/header-stamp: node-support-stamp
	cd '$(dir $@)' && for file in nodetags.h; do \
	  rm -f $$file && $(LN_S) "../../../$(subdir)/$$file" . ; \
	done
	touch $@

copyfuncs.o: copyfuncs.c copyfuncs.funcs.c copyfuncs.switch.c | node-support-stamp
equalfuncs.o: equalfuncs.c equalfuncs.funcs.c equalfuncs.switch.c | node-support-stamp
outfuncs.o: outfuncs.c outfuncs.funcs.c outfuncs.switch.c | node-support-stamp
queryjumblefuncs.o: queryjumblefuncs.c queryjumblefuncs.funcs.c queryjumblefuncs.switch.c | node-support-stamp
readfuncs.o:  readfuncs.c readfuncs.funcs.c readfuncs.switch.c | node-support-stamp

clean:
	rm -f node-support-stamp $(addsuffix funcs.funcs.c,copy equal out queryjumble read) $(addsuffix funcs.switch.c,copy equal out queryjumble read) nodetags.h
