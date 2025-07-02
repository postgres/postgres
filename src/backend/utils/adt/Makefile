#
# Makefile for utils/adt
#
# src/backend/utils/adt/Makefile
#

subdir = src/backend/utils/adt
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

# keep this list arranged alphabetically or it gets to be a mess
OBJS = \
	acl.o \
	amutils.o \
	array_expanded.o \
	array_selfuncs.o \
	array_typanalyze.o \
	array_userfuncs.o \
	arrayfuncs.o \
	arraysubs.o \
	arrayutils.o \
	ascii.o \
	bool.o \
	bytea.o \
	cash.o \
	char.o \
	cryptohashfuncs.o \
	date.o \
	datetime.o \
	datum.o \
	dbsize.o \
	domains.o \
	encode.o \
	enum.o \
	expandeddatum.o \
	expandedrecord.o \
	float.o \
	format_type.o \
	formatting.o \
	genfile.o \
	geo_ops.o \
	geo_selfuncs.o \
	geo_spgist.o \
	hbafuncs.o \
	inet_cidr_ntop.o \
	inet_net_pton.o \
	int.o \
	int8.o \
	json.o \
	jsonb.o \
	jsonb_gin.o \
	jsonb_op.o \
	jsonb_util.o \
	jsonfuncs.o \
	jsonbsubs.o \
	jsonpath.o \
	jsonpath_exec.o \
	jsonpath_gram.o \
	jsonpath_scan.o \
	like.o \
	like_support.o \
	lockfuncs.o \
	mac.o \
	mac8.o \
	mcxtfuncs.o \
	misc.o \
	multirangetypes.o \
	multirangetypes_selfuncs.o \
	name.o \
	network.o \
	network_gist.o \
	network_selfuncs.o \
	network_spgist.o \
	numeric.o \
	numutils.o \
	oid.o \
	oracle_compat.o \
	orderedsetaggs.o \
	partitionfuncs.o \
	pg_locale.o \
	pg_locale_builtin.o \
	pg_locale_icu.o \
	pg_locale_libc.o \
	pg_lsn.o \
	pg_upgrade_support.o \
	pgstatfuncs.o \
	pseudorandomfuncs.o \
	pseudotypes.o \
	quote.o \
	rangetypes.o \
	rangetypes_gist.o \
	rangetypes_selfuncs.o \
	rangetypes_spgist.o \
	rangetypes_typanalyze.o \
	regexp.o \
	regproc.o \
	ri_triggers.o \
	rowtypes.o \
	ruleutils.o \
	selfuncs.o \
	skipsupport.o \
	tid.o \
	timestamp.o \
	trigfuncs.o \
	tsginidx.o \
	tsgistidx.o \
	tsquery.o \
	tsquery_cleanup.o \
	tsquery_gist.o \
	tsquery_op.o \
	tsquery_rewrite.o \
	tsquery_util.o \
	tsrank.o \
	tsvector.o \
	tsvector_op.o \
	tsvector_parser.o \
	uuid.o \
	varbit.o \
	varchar.o \
	varlena.o \
	version.o \
	waitfuncs.o \
	windowfuncs.o \
	xid.o \
	xid8funcs.o \
	xml.o

# See notes in src/backend/parser/Makefile about the following two rules
jsonpath_gram.h: jsonpath_gram.c
	touch $@

jsonpath_gram.c: BISONFLAGS += -d

jsonpath_scan.c: FLEXFLAGS = -CF -p -p
jsonpath_scan.c: FLEX_NO_BACKUP=yes

# Force these dependencies to be known even without dependency info built:
jsonpath_gram.o jsonpath_scan.o: jsonpath_gram.h

clean:
	rm -f lex.backup
	rm -f jsonpath_gram.c jsonpath_gram.h jsonpath_scan.c

like.o: like.c like_match.c

# Some code in numeric.c benefits from auto-vectorization
numeric.o: CFLAGS += ${CFLAGS_VECTORIZE}

varlena.o: varlena.c levenshtein.c

include $(top_srcdir)/src/backend/common.mk
