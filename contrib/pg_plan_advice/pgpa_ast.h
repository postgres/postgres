/*-------------------------------------------------------------------------
 *
 * pgpa_ast.h
 *	  abstract syntax trees for plan advice, plus parser/scanner support
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_ast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPA_AST_H
#define PGPA_AST_H

#include "pgpa_identifier.h"

#include "nodes/pg_list.h"

/*
 * Advice items generally take the form SOME_TAG(item [...]), where an item
 * can take various forms. The simplest case is a relation identifier, but
 * some tags allow sublists, and JOIN_ORDER() allows both ordered and unordered
 * sublists.
 */
typedef enum
{
	PGPA_TARGET_IDENTIFIER,		/* relation identifier */
	PGPA_TARGET_ORDERED_LIST,	/* (item ...) */
	PGPA_TARGET_UNORDERED_LIST	/* {item ...} */
} pgpa_target_type;

/*
 * An index specification.
 */
typedef struct pgpa_index_target
{
	/* Index schema and name */
	char	   *indnamespace;
	char	   *indname;
} pgpa_index_target;

/*
 * A single item about which advice is being given, which could be either
 * a relation identifier that we want to break out into its constituent fields,
 * or a sublist of some kind.
 */
typedef struct pgpa_advice_target
{
	pgpa_target_type ttype;

	/*
	 * This field is meaningful when ttype is PGPA_TARGET_IDENTIFIER.
	 *
	 * All identifiers must have an alias name and an occurrence number; the
	 * remaining fields can be NULL. Note that it's possible to specify a
	 * partition name without a partition schema, but not the reverse.
	 */
	pgpa_identifier rid;

	/*
	 * This field is set when ttype is PGPA_TARGET_IDENTIFIER and the advice
	 * tag is PGPA_TAG_INDEX_SCAN or PGPA_TAG_INDEX_ONLY_SCAN.
	 */
	pgpa_index_target *itarget;

	/*
	 * When the ttype is PGPA_TARGET_<anything>_LIST, this field contains a
	 * list of additional pgpa_advice_target objects. Otherwise, it is unused.
	 */
	List	   *children;
} pgpa_advice_target;

/*
 * These are all the kinds of advice that we know how to parse. If a keyword
 * is found at the top level, it must be in this list.
 *
 * If you change anything here, also update pgpa_parse_advice_tag and
 * pgpa_cstring_advice_tag.
 */
typedef enum pgpa_advice_tag_type
{
	PGPA_TAG_BITMAP_HEAP_SCAN,
	PGPA_TAG_FOREIGN_JOIN,
	PGPA_TAG_GATHER,
	PGPA_TAG_GATHER_MERGE,
	PGPA_TAG_HASH_JOIN,
	PGPA_TAG_INDEX_ONLY_SCAN,
	PGPA_TAG_INDEX_SCAN,
	PGPA_TAG_JOIN_ORDER,
	PGPA_TAG_MERGE_JOIN_MATERIALIZE,
	PGPA_TAG_MERGE_JOIN_PLAIN,
	PGPA_TAG_NESTED_LOOP_MATERIALIZE,
	PGPA_TAG_NESTED_LOOP_MEMOIZE,
	PGPA_TAG_NESTED_LOOP_PLAIN,
	PGPA_TAG_NO_GATHER,
	PGPA_TAG_PARTITIONWISE,
	PGPA_TAG_SEMIJOIN_NON_UNIQUE,
	PGPA_TAG_SEMIJOIN_UNIQUE,
	PGPA_TAG_SEQ_SCAN,
	PGPA_TAG_TID_SCAN
} pgpa_advice_tag_type;

/*
 * An item of advice, meaning a tag and the list of all targets to which
 * it is being applied.
 *
 * "targets" is a list of pgpa_advice_target objects.
 *
 * The List returned from pgpa_yyparse is list of pgpa_advice_item objects.
 */
typedef struct pgpa_advice_item
{
	pgpa_advice_tag_type tag;
	List	   *targets;
} pgpa_advice_item;

/*
 * Result of comparing an array of pgpa_relation_identifier objects to a
 * pgpa_advice_target.
 *
 * PGPA_ITM_EQUAL means all targets are matched by some identifier, and
 * all identifiers were matched to a target.
 *
 * PGPA_ITM_KEYS_ARE_SUBSET means that all identifiers matched to a target,
 * but there were leftover targets. Generally, this means that the advice is
 * looking to apply to all of the rels we have plus some additional ones that
 * we don't have.
 *
 * PGPA_ITM_TARGETS_ARE_SUBSET means that all targets are matched by
 * identifiers, but there were leftover identifiers. Generally, this means
 * that the advice is looking to apply to some but not all of the rels we have.
 *
 * PGPA_ITM_INTERSECTING means that some identifiers and targets were matched,
 * but neither all identifiers nor all targets could be matched to items in
 * the other set.
 *
 * PGPA_ITM_DISJOINT means that no matches between identifiers and targets were
 * found.
 */
typedef enum
{
	PGPA_ITM_EQUAL,
	PGPA_ITM_KEYS_ARE_SUBSET,
	PGPA_ITM_TARGETS_ARE_SUBSET,
	PGPA_ITM_INTERSECTING,
	PGPA_ITM_DISJOINT
} pgpa_itm_type;

/* for pgpa_scanner.l and pgpa_parser.y */
union YYSTYPE;
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif

/* in pgpa_scanner.l */
extern int	pgpa_yylex(union YYSTYPE *yylval_param, List **result,
					   char **parse_error_msg_p, yyscan_t yyscanner);
extern void pgpa_yyerror(List **result, char **parse_error_msg_p,
						 yyscan_t yyscanner,
						 const char *message);
extern void pgpa_scanner_init(const char *str, yyscan_t *yyscannerp);
extern void pgpa_scanner_finish(yyscan_t yyscanner);

/* in pgpa_parser.y */
extern int	pgpa_yyparse(List **result, char **parse_error_msg_p,
						 yyscan_t yyscanner);
extern List *pgpa_parse(const char *advice_string, char **error_p);

/* in pgpa_ast.c */
extern char *pgpa_cstring_advice_tag(pgpa_advice_tag_type advice_tag);
extern bool pgpa_identifier_matches_target(pgpa_identifier *rid,
										   pgpa_advice_target *target);
extern pgpa_itm_type pgpa_identifiers_match_target(int nrids,
												   pgpa_identifier *rids,
												   pgpa_advice_target *target);
extern bool pgpa_index_targets_equal(pgpa_index_target *i1,
									 pgpa_index_target *i2);
extern pgpa_advice_tag_type pgpa_parse_advice_tag(const char *tag, bool *fail);
extern void pgpa_format_advice_target(StringInfo str,
									  pgpa_advice_target *target);
extern void pgpa_format_index_target(StringInfo str,
									 pgpa_index_target *itarget);

#endif
