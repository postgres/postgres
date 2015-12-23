#
# Makefile for utils/adt
#
# src/backend/utils/adt/Makefile
#

subdir = src/backend/utils/adt
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# keep this list arranged alphabetically or it gets to be a mess
OBJS = acl.o arrayfuncs.o array_expanded.o array_selfuncs.o \
	array_typanalyze.o array_userfuncs.o arrayutils.o ascii.o \
	bool.o cash.o char.o date.o datetime.o datum.o dbsize.o domains.o \
	encode.o enum.o expandeddatum.o \
	float.o format_type.o formatting.o genfile.o \
	geo_ops.o geo_selfuncs.o inet_cidr_ntop.o inet_net_pton.o int.o \
	int8.o json.o jsonb.o jsonb_gin.o jsonb_op.o jsonb_util.o \
	jsonfuncs.o like.o lockfuncs.o mac.o misc.o nabstime.o name.o \
	network.o network_gist.o network_selfuncs.o \
	numeric.o numutils.o oid.o oracle_compat.o \
	orderedsetaggs.o pg_locale.o pg_lsn.o pg_upgrade_support.o \
	pgstatfuncs.o \
	pseudotypes.o quote.o rangetypes.o rangetypes_gist.o \
	rangetypes_selfuncs.o rangetypes_spgist.o rangetypes_typanalyze.o \
	regexp.o regproc.o ri_triggers.o rowtypes.o ruleutils.o \
	selfuncs.o tid.o timestamp.o trigfuncs.o \
	tsginidx.o tsgistidx.o tsquery.o tsquery_cleanup.o tsquery_gist.o \
	tsquery_op.o tsquery_rewrite.o tsquery_util.o tsrank.o \
	tsvector.o tsvector_op.o tsvector_parser.o \
	txid.o uuid.o varbit.o varchar.o varlena.o version.o \
	windowfuncs.o xid.o xml.o

like.o: like.c like_match.c

varlena.o: varlena.c levenshtein.c

include $(top_srcdir)/src/backend/common.mk
