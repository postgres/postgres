#
# Makefile for utils/adt
#
# $PostgreSQL: pgsql/src/backend/utils/adt/Makefile,v 1.68 2007/10/13 23:06:26 tgl Exp $
#

subdir = src/backend/utils/adt
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# seems to be required for some date/time stuff 1999/07/22 bjm
ifeq "$(findstring alpha,$(host_cpu))" "alpha"
ifeq "$(GCC)" "yes"
override CFLAGS+= -mieee
endif
endif

OBJS = acl.o arrayfuncs.o array_userfuncs.o arrayutils.o bool.o \
	cash.o char.o date.o datetime.o datum.o domains.o \
	enum.o float.o format_type.o \
	geo_ops.o geo_selfuncs.o int.o int8.o like.o lockfuncs.o \
	misc.o nabstime.o name.o numeric.o numutils.o \
	oid.o oracle_compat.o pseudotypes.o rowtypes.o \
	regexp.o regproc.o ruleutils.o selfuncs.o \
	tid.o timestamp.o varbit.o varchar.o varlena.o version.o xid.o \
	network.o mac.o inet_net_ntop.o inet_net_pton.o \
	ri_triggers.o pg_lzcompress.o pg_locale.o formatting.o \
	ascii.o quote.o pgstatfuncs.o encode.o dbsize.o genfile.o \
	tsginidx.o tsgistidx.o tsquery.o tsquery_cleanup.o tsquery_gist.o \
	tsquery_op.o tsquery_rewrite.o tsquery_util.o tsrank.o \
	tsvector.o tsvector_op.o tsvector_parser.o \
	txid.o uuid.o xml.o

like.o: like.c like_match.c

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
