/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/tab-complete.c
 */

/*----------------------------------------------------------------------
 * This file implements a somewhat more sophisticated readline "TAB
 * completion" in psql. It is not intended to be AI, to replace
 * learning SQL, or to relieve you from thinking about what you're
 * doing. Also it does not always give you all the syntactically legal
 * completions, only those that are the most common or the ones that
 * the programmer felt most like implementing.
 *
 * CAVEAT: Tab completion causes queries to be sent to the backend.
 * The number of tuples returned gets limited, in most default
 * installations to 1000, but if you still don't like this prospect,
 * you can turn off tab completion in your ~/.inputrc (or else
 * ${INPUTRC}) file so:
 *
 *	 $if psql
 *	 set disable-completion on
 *	 $endif
 *
 * See `man 3 readline' or `info readline' for the full details.
 *
 * BUGS:
 * - Quotes, parentheses, and other funny characters are not handled
 *	 all that gracefully.
 *----------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "input.h"
#include "tab-complete.h"

/* If we don't have this, we might as well forget about the whole thing: */
#ifdef USE_READLINE

#include <ctype.h>
#include <sys/stat.h>

#include "catalog/pg_am_d.h"
#include "catalog/pg_class_d.h"
#include "common.h"
#include "common/keywords.h"
#include "libpq-fe.h"
#include "mb/pg_wchar.h"
#include "pqexpbuffer.h"
#include "settings.h"
#include "stringutils.h"

/*
 * Ancient versions of libedit provide filename_completion_function()
 * instead of rl_filename_completion_function().  Likewise for
 * [rl_]completion_matches().
 */
#ifndef HAVE_RL_FILENAME_COMPLETION_FUNCTION
#define rl_filename_completion_function filename_completion_function
#endif

#ifndef HAVE_RL_COMPLETION_MATCHES
#define rl_completion_matches completion_matches
#endif

/*
 * Currently we assume that rl_filename_dequoting_function exists if
 * rl_filename_quoting_function does.  If that proves not to be the case,
 * we'd need to test for the former, or possibly both, in configure.
 */
#ifdef HAVE_RL_FILENAME_QUOTING_FUNCTION
#define USE_FILENAME_QUOTING_FUNCTIONS 1
#endif

/* word break characters */
#define WORD_BREAKS		"\t\n@><=;|&() "

/*
 * Since readline doesn't let us pass any state through to the tab completion
 * callback, we have to use this global variable to let get_previous_words()
 * get at the previous lines of the current command.  Ick.
 */
PQExpBuffer tab_completion_query_buf = NULL;

/*
 * In some situations, the query to find out what names are available to
 * complete with must vary depending on server version.  We handle this by
 * storing a list of queries, each tagged with the minimum server version
 * it will work for.  Each list must be stored in descending server version
 * order, so that the first satisfactory query is the one to use.
 *
 * When the query string is otherwise constant, an array of VersionedQuery
 * suffices.  Terminate the array with an entry having min_server_version = 0.
 * That entry's query string can be a query that works in all supported older
 * server versions, or NULL to give up and do no completion.
 */
typedef struct VersionedQuery
{
	int			min_server_version;
	const char *query;
} VersionedQuery;

/*
 * This struct is used to define "schema queries", which are custom-built
 * to obtain possibly-schema-qualified names of database objects.  There is
 * enough similarity in the structure that we don't want to repeat it each
 * time.  So we put the components of each query into this struct and
 * assemble them with the common boilerplate in _complete_from_query().
 *
 * We also use this struct to define queries that use completion_ref_object,
 * which is some object related to the one(s) we want to get the names of
 * (for example, the table we want the indexes of).  In that usage the
 * objects we're completing might not have a schema of their own, but the
 * reference object almost always does (passed in completion_ref_schema).
 *
 * As with VersionedQuery, we can use an array of these if the query details
 * must vary across versions.
 */
typedef struct SchemaQuery
{
	/*
	 * If not zero, minimum server version this struct applies to.  If not
	 * zero, there should be a following struct with a smaller minimum server
	 * version; use catname == NULL in the last entry if we should do nothing.
	 */
	int			min_server_version;

	/*
	 * Name of catalog or catalogs to be queried, with alias(es), eg.
	 * "pg_catalog.pg_class c".  Note that "pg_namespace n" and/or
	 * "pg_namespace nr" will be added automatically when needed.
	 */
	const char *catname;

	/*
	 * Selection condition --- only rows meeting this condition are candidates
	 * to display.  If catname mentions multiple tables, include the necessary
	 * join condition here.  For example, this might look like "c.relkind = "
	 * CppAsString2(RELKIND_RELATION).  Write NULL (not an empty string) if
	 * not needed.
	 */
	const char *selcondition;

	/*
	 * Visibility condition --- which rows are visible without schema
	 * qualification?  For example, "pg_catalog.pg_table_is_visible(c.oid)".
	 * NULL if not needed.
	 */
	const char *viscondition;

	/*
	 * Namespace --- name of field to join to pg_namespace.oid when there is
	 * schema qualification.  For example, "c.relnamespace".  NULL if we don't
	 * want to join to pg_namespace (then any schema part in the input word
	 * will be ignored).
	 */
	const char *namespace;

	/*
	 * Result --- the base object name to return.  For example, "c.relname".
	 */
	const char *result;

	/*
	 * In some cases, it's difficult to keep the query from returning the same
	 * object multiple times.  Specify use_distinct to filter out duplicates.
	 */
	bool		use_distinct;

	/*
	 * Additional literal strings (usually keywords) to be offered along with
	 * the query results.  Provide a NULL-terminated array of constant
	 * strings, or NULL if none.
	 */
	const char *const *keywords;

	/*
	 * If this query uses completion_ref_object/completion_ref_schema,
	 * populate the remaining fields, else leave them NULL.  When using this
	 * capability, catname must include the catalog that defines the
	 * completion_ref_object, and selcondition must include the join condition
	 * that connects it to the result's catalog.
	 *
	 * refname is the field that should be equated to completion_ref_object,
	 * for example "cr.relname".
	 */
	const char *refname;

	/*
	 * Visibility condition to use when completion_ref_schema is not set.  For
	 * example, "pg_catalog.pg_table_is_visible(cr.oid)".  NULL if not needed.
	 */
	const char *refviscondition;

	/*
	 * Name of field to join to pg_namespace.oid when completion_ref_schema is
	 * set.  For example, "cr.relnamespace".  NULL if we don't want to
	 * consider completion_ref_schema.
	 */
	const char *refnamespace;
} SchemaQuery;


/* Store maximum number of records we want from database queries
 * (implemented via SELECT ... LIMIT xx).
 */
static int	completion_max_records;

/*
 * Communication variables set by psql_completion (mostly in COMPLETE_WITH_FOO
 * macros) and then used by the completion callback functions.  Ugly but there
 * is no better way.
 */
static char completion_last_char;	/* last char of input word */
static const char *completion_charp;	/* to pass a string */
static const char *const *completion_charpp;	/* to pass a list of strings */
static const VersionedQuery *completion_vquery; /* to pass a VersionedQuery */
static const SchemaQuery *completion_squery;	/* to pass a SchemaQuery */
static char *completion_ref_object; /* name of reference object */
static char *completion_ref_schema; /* schema name of reference object */
static bool completion_case_sensitive;	/* completion is case sensitive */
static bool completion_verbatim;	/* completion is verbatim */
static bool completion_force_quote; /* true to force-quote filenames */

/*
 * A few macros to ease typing. You can use these to complete the given
 * string with
 * 1) The result from a query you pass it. (Perhaps one of those below?)
 *	  We support both simple and versioned queries.
 * 2) The result from a schema query you pass it.
 *	  We support both simple and versioned schema queries.
 * 3) The items from a null-pointer-terminated list (with or without
 *	  case-sensitive comparison); if the list is constant you can build it
 *	  with COMPLETE_WITH() or COMPLETE_WITH_CS().  The QUERY_LIST and
 *	  QUERY_PLUS forms combine such literal lists with a query result.
 * 4) The list of attributes of the given table (possibly schema-qualified).
 * 5) The list of arguments to the given function (possibly schema-qualified).
 *
 * The query is generally expected to return raw SQL identifiers; matching
 * to what the user typed is done in a quoting-aware fashion.  If what is
 * returned is not SQL identifiers, use one of the VERBATIM forms, in which
 * case the query results are matched to the user's text without double-quote
 * processing (so if quoting is needed, you must provide it in the query
 * results).
 */
#define COMPLETE_WITH_QUERY(query) \
	COMPLETE_WITH_QUERY_LIST(query, NULL)

#define COMPLETE_WITH_QUERY_LIST(query, list) \
do { \
	completion_charp = query; \
	completion_charpp = list; \
	completion_verbatim = false; \
	matches = rl_completion_matches(text, complete_from_query); \
} while (0)

#define COMPLETE_WITH_QUERY_PLUS(query, ...) \
do { \
	static const char *const list[] = { __VA_ARGS__, NULL }; \
	COMPLETE_WITH_QUERY_LIST(query, list); \
} while (0)

#define COMPLETE_WITH_QUERY_VERBATIM(query) \
	COMPLETE_WITH_QUERY_VERBATIM_LIST(query, NULL)

#define COMPLETE_WITH_QUERY_VERBATIM_LIST(query, list) \
do { \
	completion_charp = query; \
	completion_charpp = list; \
	completion_verbatim = true; \
	matches = rl_completion_matches(text, complete_from_query); \
} while (0)

#define COMPLETE_WITH_QUERY_VERBATIM_PLUS(query, ...) \
do { \
	static const char *const list[] = { __VA_ARGS__, NULL }; \
	COMPLETE_WITH_QUERY_VERBATIM_LIST(query, list); \
} while (0)

#define COMPLETE_WITH_VERSIONED_QUERY(query) \
	COMPLETE_WITH_VERSIONED_QUERY_LIST(query, NULL)

#define COMPLETE_WITH_VERSIONED_QUERY_LIST(query, list) \
do { \
	completion_vquery = query; \
	completion_charpp = list; \
	completion_verbatim = false; \
	matches = rl_completion_matches(text, complete_from_versioned_query); \
} while (0)

#define COMPLETE_WITH_VERSIONED_QUERY_PLUS(query, ...) \
do { \
	static const char *const list[] = { __VA_ARGS__, NULL }; \
	COMPLETE_WITH_VERSIONED_QUERY_LIST(query, list); \
} while (0)

#define COMPLETE_WITH_SCHEMA_QUERY(query) \
	COMPLETE_WITH_SCHEMA_QUERY_LIST(query, NULL)

#define COMPLETE_WITH_SCHEMA_QUERY_LIST(query, list) \
do { \
	completion_squery = &(query); \
	completion_charpp = list; \
	completion_verbatim = false; \
	matches = rl_completion_matches(text, complete_from_schema_query); \
} while (0)

#define COMPLETE_WITH_SCHEMA_QUERY_PLUS(query, ...) \
do { \
	static const char *const list[] = { __VA_ARGS__, NULL }; \
	COMPLETE_WITH_SCHEMA_QUERY_LIST(query, list); \
} while (0)

#define COMPLETE_WITH_SCHEMA_QUERY_VERBATIM(query) \
do { \
	completion_squery = &(query); \
	completion_charpp = NULL; \
	completion_verbatim = true; \
	matches = rl_completion_matches(text, complete_from_schema_query); \
} while (0)

#define COMPLETE_WITH_VERSIONED_SCHEMA_QUERY(query) \
	COMPLETE_WITH_VERSIONED_SCHEMA_QUERY_LIST(query, NULL)

#define COMPLETE_WITH_VERSIONED_SCHEMA_QUERY_LIST(query, list) \
do { \
	completion_squery = query; \
	completion_charpp = list; \
	completion_verbatim = false; \
	matches = rl_completion_matches(text, complete_from_versioned_schema_query); \
} while (0)

#define COMPLETE_WITH_VERSIONED_SCHEMA_QUERY_PLUS(query, ...) \
do { \
	static const char *const list[] = { __VA_ARGS__, NULL }; \
	COMPLETE_WITH_VERSIONED_SCHEMA_QUERY_LIST(query, list); \
} while (0)

/*
 * Caution: COMPLETE_WITH_CONST is not for general-purpose use; you probably
 * want COMPLETE_WITH() with one element, instead.
 */
#define COMPLETE_WITH_CONST(cs, con) \
do { \
	completion_case_sensitive = (cs); \
	completion_charp = (con); \
	matches = rl_completion_matches(text, complete_from_const); \
} while (0)

#define COMPLETE_WITH_LIST_INT(cs, list) \
do { \
	completion_case_sensitive = (cs); \
	completion_charpp = (list); \
	matches = rl_completion_matches(text, complete_from_list); \
} while (0)

#define COMPLETE_WITH_LIST(list) COMPLETE_WITH_LIST_INT(false, list)
#define COMPLETE_WITH_LIST_CS(list) COMPLETE_WITH_LIST_INT(true, list)

#define COMPLETE_WITH(...) \
do { \
	static const char *const list[] = { __VA_ARGS__, NULL }; \
	COMPLETE_WITH_LIST(list); \
} while (0)

#define COMPLETE_WITH_CS(...) \
do { \
	static const char *const list[] = { __VA_ARGS__, NULL }; \
	COMPLETE_WITH_LIST_CS(list); \
} while (0)

#define COMPLETE_WITH_ATTR(relation) \
	COMPLETE_WITH_ATTR_LIST(relation, NULL)

#define COMPLETE_WITH_ATTR_LIST(relation, list) \
do { \
	set_completion_reference(relation); \
	completion_squery = &(Query_for_list_of_attributes); \
	completion_charpp = list; \
	completion_verbatim = false; \
	matches = rl_completion_matches(text, complete_from_schema_query); \
} while (0)

#define COMPLETE_WITH_ATTR_PLUS(relation, ...) \
do { \
	static const char *const list[] = { __VA_ARGS__, NULL }; \
	COMPLETE_WITH_ATTR_LIST(relation, list); \
} while (0)

/*
 * libedit will typically include the literal's leading single quote in
 * "text", while readline will not.  Adapt our offered strings to fit.
 * But include a quote if there's not one just before "text", to get the
 * user off to the right start.
 */
#define COMPLETE_WITH_ENUM_VALUE(type) \
do { \
	set_completion_reference(type); \
	if (text[0] == '\'' || \
		start == 0 || rl_line_buffer[start - 1] != '\'') \
		completion_squery = &(Query_for_list_of_enum_values_quoted); \
	else \
		completion_squery = &(Query_for_list_of_enum_values_unquoted); \
	completion_charpp = NULL; \
	completion_verbatim = true; \
	matches = rl_completion_matches(text, complete_from_schema_query); \
} while (0)

/*
 * Timezone completion is mostly like enum label completion, but we work
 * a little harder since this is a more common use-case.
 */
#define COMPLETE_WITH_TIMEZONE_NAME() \
do { \
	static const char *const list[] = { "DEFAULT", NULL }; \
	if (text[0] == '\'') \
		completion_charp = Query_for_list_of_timezone_names_quoted_in; \
	else if (start == 0 || rl_line_buffer[start - 1] != '\'') \
		completion_charp = Query_for_list_of_timezone_names_quoted_out; \
	else \
		completion_charp = Query_for_list_of_timezone_names_unquoted; \
	completion_charpp = list;							  \
	completion_verbatim = true; \
	matches = rl_completion_matches(text, complete_from_query); \
} while (0)

#define COMPLETE_WITH_FUNCTION_ARG(function) \
do { \
	set_completion_reference(function); \
	completion_squery = &(Query_for_list_of_arguments); \
	completion_charpp = NULL; \
	completion_verbatim = true; \
	matches = rl_completion_matches(text, complete_from_schema_query); \
} while (0)

/*
 * Assembly instructions for schema queries
 *
 * Note that toast tables are not included in those queries to avoid
 * unnecessary bloat in the completions generated.
 */

static const SchemaQuery Query_for_constraint_of_table = {
	.catname = "pg_catalog.pg_constraint con, pg_catalog.pg_class c1",
	.selcondition = "con.conrelid=c1.oid",
	.result = "con.conname",
	.refname = "c1.relname",
	.refviscondition = "pg_catalog.pg_table_is_visible(c1.oid)",
	.refnamespace = "c1.relnamespace",
};

static const SchemaQuery Query_for_constraint_of_table_not_validated = {
	.catname = "pg_catalog.pg_constraint con, pg_catalog.pg_class c1",
	.selcondition = "con.conrelid=c1.oid and not con.convalidated",
	.result = "con.conname",
	.refname = "c1.relname",
	.refviscondition = "pg_catalog.pg_table_is_visible(c1.oid)",
	.refnamespace = "c1.relnamespace",
};

static const SchemaQuery Query_for_constraint_of_type = {
	.catname = "pg_catalog.pg_constraint con, pg_catalog.pg_type t",
	.selcondition = "con.contypid=t.oid",
	.result = "con.conname",
	.refname = "t.typname",
	.refviscondition = "pg_catalog.pg_type_is_visible(t.oid)",
	.refnamespace = "t.typnamespace",
};

static const SchemaQuery Query_for_index_of_table = {
	.catname = "pg_catalog.pg_class c1, pg_catalog.pg_class c2, pg_catalog.pg_index i",
	.selcondition = "c1.oid=i.indrelid and i.indexrelid=c2.oid",
	.result = "c2.relname",
	.refname = "c1.relname",
	.refviscondition = "pg_catalog.pg_table_is_visible(c1.oid)",
	.refnamespace = "c1.relnamespace",
};

static const SchemaQuery Query_for_unique_index_of_table = {
	.catname = "pg_catalog.pg_class c1, pg_catalog.pg_class c2, pg_catalog.pg_index i",
	.selcondition = "c1.oid=i.indrelid and i.indexrelid=c2.oid and i.indisunique",
	.result = "c2.relname",
	.refname = "c1.relname",
	.refviscondition = "pg_catalog.pg_table_is_visible(c1.oid)",
	.refnamespace = "c1.relnamespace",
};

static const SchemaQuery Query_for_list_of_aggregates[] = {
	{
		.min_server_version = 110000,
		.catname = "pg_catalog.pg_proc p",
		.selcondition = "p.prokind = 'a'",
		.viscondition = "pg_catalog.pg_function_is_visible(p.oid)",
		.namespace = "p.pronamespace",
		.result = "p.proname",
	},
	{
		.catname = "pg_catalog.pg_proc p",
		.selcondition = "p.proisagg",
		.viscondition = "pg_catalog.pg_function_is_visible(p.oid)",
		.namespace = "p.pronamespace",
		.result = "p.proname",
	}
};

static const SchemaQuery Query_for_list_of_arguments = {
	.catname = "pg_catalog.pg_proc p",
	.result = "pg_catalog.oidvectortypes(p.proargtypes)||')'",
	.refname = "p.proname",
	.refviscondition = "pg_catalog.pg_function_is_visible(p.oid)",
	.refnamespace = "p.pronamespace",
};

static const SchemaQuery Query_for_list_of_attributes = {
	.catname = "pg_catalog.pg_attribute a, pg_catalog.pg_class c",
	.selcondition = "c.oid = a.attrelid and a.attnum > 0 and not a.attisdropped",
	.result = "a.attname",
	.refname = "c.relname",
	.refviscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.refnamespace = "c.relnamespace",
};

static const SchemaQuery Query_for_list_of_attribute_numbers = {
	.catname = "pg_catalog.pg_attribute a, pg_catalog.pg_class c",
	.selcondition = "c.oid = a.attrelid and a.attnum > 0 and not a.attisdropped",
	.result = "a.attnum::pg_catalog.text",
	.refname = "c.relname",
	.refviscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.refnamespace = "c.relnamespace",
};

static const char *const Keywords_for_list_of_datatypes[] = {
	"bigint",
	"boolean",
	"character",
	"double precision",
	"integer",
	"real",
	"smallint",

	/*
	 * Note: currently there's no value in offering the following multiword
	 * type names, because tab completion cannot succeed for them: we can't
	 * disambiguate until somewhere in the second word, at which point we
	 * won't have the first word as context.  ("double precision" does work,
	 * as long as no other type name begins with "double".)  Leave them out to
	 * encourage users to use the PG-specific aliases, which we can complete.
	 */
#ifdef NOT_USED
	"bit varying",
	"character varying",
	"time with time zone",
	"time without time zone",
	"timestamp with time zone",
	"timestamp without time zone",
#endif
	NULL
};

static const SchemaQuery Query_for_list_of_datatypes = {
	.catname = "pg_catalog.pg_type t",
	/* selcondition --- ignore table rowtypes and array types */
	.selcondition = "(t.typrelid = 0 "
	" OR (SELECT c.relkind = " CppAsString2(RELKIND_COMPOSITE_TYPE)
	"     FROM pg_catalog.pg_class c WHERE c.oid = t.typrelid)) "
	"AND t.typname !~ '^_'",
	.viscondition = "pg_catalog.pg_type_is_visible(t.oid)",
	.namespace = "t.typnamespace",
	.result = "t.typname",
	.keywords = Keywords_for_list_of_datatypes,
};

static const SchemaQuery Query_for_list_of_composite_datatypes = {
	.catname = "pg_catalog.pg_type t",
	/* selcondition --- only get composite types */
	.selcondition = "(SELECT c.relkind = " CppAsString2(RELKIND_COMPOSITE_TYPE)
	" FROM pg_catalog.pg_class c WHERE c.oid = t.typrelid) "
	"AND t.typname !~ '^_'",
	.viscondition = "pg_catalog.pg_type_is_visible(t.oid)",
	.namespace = "t.typnamespace",
	.result = "t.typname",
};

static const SchemaQuery Query_for_list_of_domains = {
	.catname = "pg_catalog.pg_type t",
	.selcondition = "t.typtype = 'd'",
	.viscondition = "pg_catalog.pg_type_is_visible(t.oid)",
	.namespace = "t.typnamespace",
	.result = "t.typname",
};

static const SchemaQuery Query_for_list_of_enum_values_quoted = {
	.catname = "pg_catalog.pg_enum e, pg_catalog.pg_type t",
	.selcondition = "t.oid = e.enumtypid",
	.result = "pg_catalog.quote_literal(enumlabel)",
	.refname = "t.typname",
	.refviscondition = "pg_catalog.pg_type_is_visible(t.oid)",
	.refnamespace = "t.typnamespace",
};

static const SchemaQuery Query_for_list_of_enum_values_unquoted = {
	.catname = "pg_catalog.pg_enum e, pg_catalog.pg_type t",
	.selcondition = "t.oid = e.enumtypid",
	.result = "e.enumlabel",
	.refname = "t.typname",
	.refviscondition = "pg_catalog.pg_type_is_visible(t.oid)",
	.refnamespace = "t.typnamespace",
};

/* Note: this intentionally accepts aggregates as well as plain functions */
static const SchemaQuery Query_for_list_of_functions[] = {
	{
		.min_server_version = 110000,
		.catname = "pg_catalog.pg_proc p",
		.selcondition = "p.prokind != 'p'",
		.viscondition = "pg_catalog.pg_function_is_visible(p.oid)",
		.namespace = "p.pronamespace",
		.result = "p.proname",
	},
	{
		.catname = "pg_catalog.pg_proc p",
		.viscondition = "pg_catalog.pg_function_is_visible(p.oid)",
		.namespace = "p.pronamespace",
		.result = "p.proname",
	}
};

static const SchemaQuery Query_for_list_of_procedures[] = {
	{
		.min_server_version = 110000,
		.catname = "pg_catalog.pg_proc p",
		.selcondition = "p.prokind = 'p'",
		.viscondition = "pg_catalog.pg_function_is_visible(p.oid)",
		.namespace = "p.pronamespace",
		.result = "p.proname",
	},
	{
		/* not supported in older versions */
		.catname = NULL,
	}
};

static const SchemaQuery Query_for_list_of_routines = {
	.catname = "pg_catalog.pg_proc p",
	.viscondition = "pg_catalog.pg_function_is_visible(p.oid)",
	.namespace = "p.pronamespace",
	.result = "p.proname",
};

static const SchemaQuery Query_for_list_of_sequences = {
	.catname = "pg_catalog.pg_class c",
	.selcondition = "c.relkind IN (" CppAsString2(RELKIND_SEQUENCE) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

static const SchemaQuery Query_for_list_of_foreign_tables = {
	.catname = "pg_catalog.pg_class c",
	.selcondition = "c.relkind IN (" CppAsString2(RELKIND_FOREIGN_TABLE) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

static const SchemaQuery Query_for_list_of_tables = {
	.catname = "pg_catalog.pg_class c",
	.selcondition =
	"c.relkind IN (" CppAsString2(RELKIND_RELATION) ", "
	CppAsString2(RELKIND_PARTITIONED_TABLE) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

static const SchemaQuery Query_for_list_of_partitioned_tables = {
	.catname = "pg_catalog.pg_class c",
	.selcondition = "c.relkind IN (" CppAsString2(RELKIND_PARTITIONED_TABLE) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

static const SchemaQuery Query_for_list_of_tables_for_constraint = {
	.catname = "pg_catalog.pg_class c, pg_catalog.pg_constraint con",
	.selcondition = "c.oid=con.conrelid and c.relkind IN ("
	CppAsString2(RELKIND_RELATION) ", "
	CppAsString2(RELKIND_PARTITIONED_TABLE) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
	.use_distinct = true,
	.refname = "con.conname",
};

static const SchemaQuery Query_for_list_of_tables_for_policy = {
	.catname = "pg_catalog.pg_class c, pg_catalog.pg_policy p",
	.selcondition = "c.oid=p.polrelid",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
	.use_distinct = true,
	.refname = "p.polname",
};

static const SchemaQuery Query_for_list_of_tables_for_rule = {
	.catname = "pg_catalog.pg_class c, pg_catalog.pg_rewrite r",
	.selcondition = "c.oid=r.ev_class",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
	.use_distinct = true,
	.refname = "r.rulename",
};

static const SchemaQuery Query_for_list_of_tables_for_trigger = {
	.catname = "pg_catalog.pg_class c, pg_catalog.pg_trigger t",
	.selcondition = "c.oid=t.tgrelid",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
	.use_distinct = true,
	.refname = "t.tgname",
};

static const SchemaQuery Query_for_list_of_ts_configurations = {
	.catname = "pg_catalog.pg_ts_config c",
	.viscondition = "pg_catalog.pg_ts_config_is_visible(c.oid)",
	.namespace = "c.cfgnamespace",
	.result = "c.cfgname",
};

static const SchemaQuery Query_for_list_of_ts_dictionaries = {
	.catname = "pg_catalog.pg_ts_dict d",
	.viscondition = "pg_catalog.pg_ts_dict_is_visible(d.oid)",
	.namespace = "d.dictnamespace",
	.result = "d.dictname",
};

static const SchemaQuery Query_for_list_of_ts_parsers = {
	.catname = "pg_catalog.pg_ts_parser p",
	.viscondition = "pg_catalog.pg_ts_parser_is_visible(p.oid)",
	.namespace = "p.prsnamespace",
	.result = "p.prsname",
};

static const SchemaQuery Query_for_list_of_ts_templates = {
	.catname = "pg_catalog.pg_ts_template t",
	.viscondition = "pg_catalog.pg_ts_template_is_visible(t.oid)",
	.namespace = "t.tmplnamespace",
	.result = "t.tmplname",
};

static const SchemaQuery Query_for_list_of_views = {
	.catname = "pg_catalog.pg_class c",
	.selcondition = "c.relkind IN (" CppAsString2(RELKIND_VIEW) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

static const SchemaQuery Query_for_list_of_matviews = {
	.catname = "pg_catalog.pg_class c",
	.selcondition = "c.relkind IN (" CppAsString2(RELKIND_MATVIEW) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

static const SchemaQuery Query_for_list_of_indexes = {
	.catname = "pg_catalog.pg_class c",
	.selcondition =
	"c.relkind IN (" CppAsString2(RELKIND_INDEX) ", "
	CppAsString2(RELKIND_PARTITIONED_INDEX) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

static const SchemaQuery Query_for_list_of_partitioned_indexes = {
	.catname = "pg_catalog.pg_class c",
	.selcondition = "c.relkind = " CppAsString2(RELKIND_PARTITIONED_INDEX),
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};


/* All relations */
static const SchemaQuery Query_for_list_of_relations = {
	.catname = "pg_catalog.pg_class c",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

/* partitioned relations */
static const SchemaQuery Query_for_list_of_partitioned_relations = {
	.catname = "pg_catalog.pg_class c",
	.selcondition = "c.relkind IN (" CppAsString2(RELKIND_PARTITIONED_TABLE)
	", " CppAsString2(RELKIND_PARTITIONED_INDEX) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

static const SchemaQuery Query_for_list_of_operator_families = {
	.catname = "pg_catalog.pg_opfamily c",
	.viscondition = "pg_catalog.pg_opfamily_is_visible(c.oid)",
	.namespace = "c.opfnamespace",
	.result = "c.opfname",
};

/* Relations supporting INSERT, UPDATE or DELETE */
static const SchemaQuery Query_for_list_of_updatables = {
	.catname = "pg_catalog.pg_class c",
	.selcondition =
	"c.relkind IN (" CppAsString2(RELKIND_RELATION) ", "
	CppAsString2(RELKIND_FOREIGN_TABLE) ", "
	CppAsString2(RELKIND_VIEW) ", "
	CppAsString2(RELKIND_PARTITIONED_TABLE) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

/* Relations supporting MERGE */
static const SchemaQuery Query_for_list_of_mergetargets = {
	.catname = "pg_catalog.pg_class c",
	.selcondition =
	"c.relkind IN (" CppAsString2(RELKIND_RELATION) ", "
	CppAsString2(RELKIND_VIEW) ", "
	CppAsString2(RELKIND_PARTITIONED_TABLE) ") ",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

/* Relations supporting SELECT */
static const SchemaQuery Query_for_list_of_selectables = {
	.catname = "pg_catalog.pg_class c",
	.selcondition =
	"c.relkind IN (" CppAsString2(RELKIND_RELATION) ", "
	CppAsString2(RELKIND_SEQUENCE) ", "
	CppAsString2(RELKIND_VIEW) ", "
	CppAsString2(RELKIND_MATVIEW) ", "
	CppAsString2(RELKIND_FOREIGN_TABLE) ", "
	CppAsString2(RELKIND_PARTITIONED_TABLE) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

/* Relations supporting TRUNCATE */
static const SchemaQuery Query_for_list_of_truncatables = {
	.catname = "pg_catalog.pg_class c",
	.selcondition =
	"c.relkind IN (" CppAsString2(RELKIND_RELATION) ", "
	CppAsString2(RELKIND_FOREIGN_TABLE) ", "
	CppAsString2(RELKIND_PARTITIONED_TABLE) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

/* Relations supporting GRANT are currently same as those supporting SELECT */
#define Query_for_list_of_grantables Query_for_list_of_selectables

/* Relations supporting ANALYZE */
static const SchemaQuery Query_for_list_of_analyzables = {
	.catname = "pg_catalog.pg_class c",
	.selcondition =
	"c.relkind IN (" CppAsString2(RELKIND_RELATION) ", "
	CppAsString2(RELKIND_PARTITIONED_TABLE) ", "
	CppAsString2(RELKIND_MATVIEW) ", "
	CppAsString2(RELKIND_FOREIGN_TABLE) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

/* Relations supporting index creation */
static const SchemaQuery Query_for_list_of_indexables = {
	.catname = "pg_catalog.pg_class c",
	.selcondition =
	"c.relkind IN (" CppAsString2(RELKIND_RELATION) ", "
	CppAsString2(RELKIND_PARTITIONED_TABLE) ", "
	CppAsString2(RELKIND_MATVIEW) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

/*
 * Relations supporting VACUUM are currently same as those supporting
 * indexing.
 */
#define Query_for_list_of_vacuumables Query_for_list_of_indexables

/* Relations supporting CLUSTER */
static const SchemaQuery Query_for_list_of_clusterables = {
	.catname = "pg_catalog.pg_class c",
	.selcondition =
	"c.relkind IN (" CppAsString2(RELKIND_RELATION) ", "
	CppAsString2(RELKIND_PARTITIONED_TABLE) ", "
	CppAsString2(RELKIND_MATVIEW) ")",
	.viscondition = "pg_catalog.pg_table_is_visible(c.oid)",
	.namespace = "c.relnamespace",
	.result = "c.relname",
};

static const SchemaQuery Query_for_list_of_constraints_with_schema = {
	.catname = "pg_catalog.pg_constraint c",
	.selcondition = "c.conrelid <> 0",
	.namespace = "c.connamespace",
	.result = "c.conname",
};

static const SchemaQuery Query_for_list_of_statistics = {
	.catname = "pg_catalog.pg_statistic_ext s",
	.viscondition = "pg_catalog.pg_statistics_obj_is_visible(s.oid)",
	.namespace = "s.stxnamespace",
	.result = "s.stxname",
};

static const SchemaQuery Query_for_list_of_collations = {
	.catname = "pg_catalog.pg_collation c",
	.selcondition = "c.collencoding IN (-1, pg_catalog.pg_char_to_encoding(pg_catalog.getdatabaseencoding()))",
	.viscondition = "pg_catalog.pg_collation_is_visible(c.oid)",
	.namespace = "c.collnamespace",
	.result = "c.collname",
};

static const SchemaQuery Query_for_partition_of_table = {
	.catname = "pg_catalog.pg_class c1, pg_catalog.pg_class c2, pg_catalog.pg_inherits i",
	.selcondition = "c1.oid=i.inhparent and i.inhrelid=c2.oid and c2.relispartition",
	.viscondition = "pg_catalog.pg_table_is_visible(c2.oid)",
	.namespace = "c2.relnamespace",
	.result = "c2.relname",
	.refname = "c1.relname",
	.refviscondition = "pg_catalog.pg_table_is_visible(c1.oid)",
	.refnamespace = "c1.relnamespace",
};

static const SchemaQuery Query_for_rule_of_table = {
	.catname = "pg_catalog.pg_rewrite r, pg_catalog.pg_class c1",
	.selcondition = "r.ev_class=c1.oid",
	.result = "r.rulename",
	.refname = "c1.relname",
	.refviscondition = "pg_catalog.pg_table_is_visible(c1.oid)",
	.refnamespace = "c1.relnamespace",
};

static const SchemaQuery Query_for_trigger_of_table = {
	.catname = "pg_catalog.pg_trigger t, pg_catalog.pg_class c1",
	.selcondition = "t.tgrelid=c1.oid and not t.tgisinternal",
	.result = "t.tgname",
	.refname = "c1.relname",
	.refviscondition = "pg_catalog.pg_table_is_visible(c1.oid)",
	.refnamespace = "c1.relnamespace",
};


/*
 * Queries to get lists of names of various kinds of things, possibly
 * restricted to names matching a partially entered name.  Don't use
 * this method where the user might wish to enter a schema-qualified
 * name; make a SchemaQuery instead.
 *
 * In these queries, there must be a restriction clause of the form
 *		output LIKE '%s'
 * where "output" is the same string that the query returns.  The %s
 * will be replaced by a LIKE pattern to match the already-typed text.
 *
 * There can be a second '%s', which will be replaced by a suitably-escaped
 * version of the string provided in completion_ref_object.  If there is a
 * third '%s', it will be replaced by a suitably-escaped version of the string
 * provided in completion_ref_schema.  NOTE: using completion_ref_object
 * that way is usually the wrong thing, and using completion_ref_schema
 * that way is always the wrong thing.  Make a SchemaQuery instead.
 */

#define Query_for_list_of_template_databases \
"SELECT d.datname "\
"  FROM pg_catalog.pg_database d "\
" WHERE d.datname LIKE '%s' "\
"   AND (d.datistemplate OR pg_catalog.pg_has_role(d.datdba, 'USAGE'))"

#define Query_for_list_of_databases \
"SELECT datname FROM pg_catalog.pg_database "\
" WHERE datname LIKE '%s'"

#define Query_for_list_of_tablespaces \
"SELECT spcname FROM pg_catalog.pg_tablespace "\
" WHERE spcname LIKE '%s'"

#define Query_for_list_of_encodings \
" SELECT DISTINCT pg_catalog.pg_encoding_to_char(conforencoding) "\
"   FROM pg_catalog.pg_conversion "\
"  WHERE pg_catalog.pg_encoding_to_char(conforencoding) LIKE pg_catalog.upper('%s')"

#define Query_for_list_of_languages \
"SELECT lanname "\
"  FROM pg_catalog.pg_language "\
" WHERE lanname != 'internal' "\
"   AND lanname LIKE '%s'"

#define Query_for_list_of_schemas \
"SELECT nspname FROM pg_catalog.pg_namespace "\
" WHERE nspname LIKE '%s'"

/* Use COMPLETE_WITH_QUERY_VERBATIM with these queries for GUC names: */
#define Query_for_list_of_alter_system_set_vars \
"SELECT pg_catalog.lower(name) FROM pg_catalog.pg_settings "\
" WHERE context != 'internal' "\
"   AND pg_catalog.lower(name) LIKE pg_catalog.lower('%s')"

#define Query_for_list_of_set_vars \
"SELECT pg_catalog.lower(name) FROM pg_catalog.pg_settings "\
" WHERE context IN ('user', 'superuser') "\
"   AND pg_catalog.lower(name) LIKE pg_catalog.lower('%s')"

#define Query_for_list_of_show_vars \
"SELECT pg_catalog.lower(name) FROM pg_catalog.pg_settings "\
" WHERE pg_catalog.lower(name) LIKE pg_catalog.lower('%s')"

#define Query_for_list_of_roles \
" SELECT rolname "\
"   FROM pg_catalog.pg_roles "\
"  WHERE rolname LIKE '%s'"

/* add these to Query_for_list_of_roles in OWNER contexts */
#define Keywords_for_list_of_owner_roles \
"CURRENT_ROLE", "CURRENT_USER", "SESSION_USER"

/* add these to Query_for_list_of_roles in GRANT contexts */
#define Keywords_for_list_of_grant_roles \
Keywords_for_list_of_owner_roles, "PUBLIC"

#define Query_for_all_table_constraints \
"SELECT conname "\
"  FROM pg_catalog.pg_constraint c "\
" WHERE c.conrelid <> 0 "\
"       and conname LIKE '%s'"

#define Query_for_list_of_fdws \
" SELECT fdwname "\
"   FROM pg_catalog.pg_foreign_data_wrapper "\
"  WHERE fdwname LIKE '%s'"

#define Query_for_list_of_servers \
" SELECT srvname "\
"   FROM pg_catalog.pg_foreign_server "\
"  WHERE srvname LIKE '%s'"

#define Query_for_list_of_user_mappings \
" SELECT usename "\
"   FROM pg_catalog.pg_user_mappings "\
"  WHERE usename LIKE '%s'"

#define Query_for_list_of_access_methods \
" SELECT amname "\
"   FROM pg_catalog.pg_am "\
"  WHERE amname LIKE '%s'"

#define Query_for_list_of_index_access_methods \
" SELECT amname "\
"   FROM pg_catalog.pg_am "\
"  WHERE amname LIKE '%s' AND "\
"   amtype=" CppAsString2(AMTYPE_INDEX)

#define Query_for_list_of_table_access_methods \
" SELECT amname "\
"   FROM pg_catalog.pg_am "\
"  WHERE amname LIKE '%s' AND "\
"   amtype=" CppAsString2(AMTYPE_TABLE)

#define Query_for_list_of_extensions \
" SELECT extname "\
"   FROM pg_catalog.pg_extension "\
"  WHERE extname LIKE '%s'"

#define Query_for_list_of_available_extensions \
" SELECT name "\
"   FROM pg_catalog.pg_available_extensions "\
"  WHERE name LIKE '%s' AND installed_version IS NULL"

#define Query_for_list_of_available_extension_versions \
" SELECT version "\
"   FROM pg_catalog.pg_available_extension_versions "\
"  WHERE version LIKE '%s' AND name='%s'"

#define Query_for_list_of_prepared_statements \
" SELECT name "\
"   FROM pg_catalog.pg_prepared_statements "\
"  WHERE name LIKE '%s'"

#define Query_for_list_of_event_triggers \
" SELECT evtname "\
"   FROM pg_catalog.pg_event_trigger "\
"  WHERE evtname LIKE '%s'"

#define Query_for_list_of_tablesample_methods \
" SELECT proname "\
"   FROM pg_catalog.pg_proc "\
"  WHERE prorettype = 'pg_catalog.tsm_handler'::pg_catalog.regtype AND "\
"        proargtypes[0] = 'pg_catalog.internal'::pg_catalog.regtype AND "\
"        proname LIKE '%s'"

#define Query_for_list_of_policies \
" SELECT polname "\
"   FROM pg_catalog.pg_policy "\
"  WHERE polname LIKE '%s'"

#define Query_for_values_of_enum_GUC \
" SELECT val FROM ( "\
"   SELECT name, pg_catalog.unnest(enumvals) AS val "\
"     FROM pg_catalog.pg_settings "\
"    ) ss "\
"  WHERE val LIKE '%s'"\
"        and pg_catalog.lower(name)=pg_catalog.lower('%s')"

#define Query_for_list_of_channels \
" SELECT channel "\
"   FROM pg_catalog.pg_listening_channels() AS channel "\
"  WHERE channel LIKE '%s'"

#define Query_for_list_of_cursors \
" SELECT name "\
"   FROM pg_catalog.pg_cursors "\
"  WHERE name LIKE '%s'"

#define Query_for_list_of_timezone_names_unquoted \
" SELECT name "\
"   FROM pg_catalog.pg_timezone_names() "\
"  WHERE pg_catalog.lower(name) LIKE pg_catalog.lower('%s')"

#define Query_for_list_of_timezone_names_quoted_out \
"SELECT pg_catalog.quote_literal(name) AS name "\
"  FROM pg_catalog.pg_timezone_names() "\
" WHERE pg_catalog.lower(name) LIKE pg_catalog.lower('%s')"

#define Query_for_list_of_timezone_names_quoted_in \
"SELECT pg_catalog.quote_literal(name) AS name "\
"  FROM pg_catalog.pg_timezone_names() "\
" WHERE pg_catalog.quote_literal(pg_catalog.lower(name)) LIKE pg_catalog.lower('%s')"

/* Privilege options shared between GRANT and REVOKE */
#define Privilege_options_of_grant_and_revoke \
"SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE", "REFERENCES", "TRIGGER", \
"CREATE", "CONNECT", "TEMPORARY", "EXECUTE", "USAGE", "SET", "ALTER SYSTEM", \
"MAINTAIN", "ALL"

/* ALTER PROCEDURE options */
#define Alter_procedure_options \
"DEPENDS ON EXTENSION", "EXTERNAL SECURITY", "NO DEPENDS ON EXTENSION", \
"OWNER TO", "RENAME TO", "RESET", "SECURITY", "SET"

/* ALTER ROUTINE options */
#define Alter_routine_options \
Alter_procedure_options, "COST", "IMMUTABLE", "LEAKPROOF", "NOT LEAKPROOF", \
"PARALLEL", "ROWS", "STABLE", "VOLATILE"

/* ALTER FUNCTION options */
#define Alter_function_options \
Alter_routine_options, "CALLED ON NULL INPUT", "RETURNS NULL ON NULL INPUT", \
"STRICT", "SUPPORT"

/*
 * These object types were introduced later than our support cutoff of
 * server version 9.2.  We use the VersionedQuery infrastructure so that
 * we don't send certain-to-fail queries to older servers.
 */

static const VersionedQuery Query_for_list_of_publications[] = {
	{100000,
		" SELECT pubname "
		"   FROM pg_catalog.pg_publication "
		"  WHERE pubname LIKE '%s'"
	},
	{0, NULL}
};

static const VersionedQuery Query_for_list_of_subscriptions[] = {
	{100000,
		" SELECT s.subname "
		"   FROM pg_catalog.pg_subscription s, pg_catalog.pg_database d "
		"  WHERE s.subname LIKE '%s' "
		"    AND d.datname = pg_catalog.current_database() "
		"    AND s.subdbid = d.oid"
	},
	{0, NULL}
};

/*
 * This is a list of all "things" in Pgsql, which can show up after CREATE or
 * DROP; and there is also a query to get a list of them.
 */

typedef struct
{
	const char *name;
	/* Provide at most one of these three types of query: */
	const char *query;			/* simple query, or NULL */
	const VersionedQuery *vquery;	/* versioned query, or NULL */
	const SchemaQuery *squery;	/* schema query, or NULL */
	const char *const *keywords;	/* keywords to be offered as well */
	const bits32 flags;			/* visibility flags, see below */
} pgsql_thing_t;

#define THING_NO_CREATE		(1 << 0)	/* should not show up after CREATE */
#define THING_NO_DROP		(1 << 1)	/* should not show up after DROP */
#define THING_NO_ALTER		(1 << 2)	/* should not show up after ALTER */
#define THING_NO_SHOW		(THING_NO_CREATE | THING_NO_DROP | THING_NO_ALTER)

/* When we have DROP USER etc, also offer MAPPING FOR */
static const char *const Keywords_for_user_thing[] = {
	"MAPPING FOR",
	NULL
};

static const pgsql_thing_t words_after_create[] = {
	{"ACCESS METHOD", NULL, NULL, NULL, NULL, THING_NO_ALTER},
	{"AGGREGATE", NULL, NULL, Query_for_list_of_aggregates},
	{"CAST", NULL, NULL, NULL}, /* Casts have complex structures for names, so
								 * skip it */
	{"COLLATION", NULL, NULL, &Query_for_list_of_collations},

	/*
	 * CREATE CONSTRAINT TRIGGER is not supported here because it is designed
	 * to be used only by pg_dump.
	 */
	{"CONFIGURATION", NULL, NULL, &Query_for_list_of_ts_configurations, NULL, THING_NO_SHOW},
	{"CONVERSION", "SELECT conname FROM pg_catalog.pg_conversion WHERE conname LIKE '%s'"},
	{"DATABASE", Query_for_list_of_databases},
	{"DEFAULT PRIVILEGES", NULL, NULL, NULL, NULL, THING_NO_CREATE | THING_NO_DROP},
	{"DICTIONARY", NULL, NULL, &Query_for_list_of_ts_dictionaries, NULL, THING_NO_SHOW},
	{"DOMAIN", NULL, NULL, &Query_for_list_of_domains},
	{"EVENT TRIGGER", NULL, NULL, NULL},
	{"EXTENSION", Query_for_list_of_extensions},
	{"FOREIGN DATA WRAPPER", NULL, NULL, NULL},
	{"FOREIGN TABLE", NULL, NULL, NULL},
	{"FUNCTION", NULL, NULL, Query_for_list_of_functions},
	{"GROUP", Query_for_list_of_roles},
	{"INDEX", NULL, NULL, &Query_for_list_of_indexes},
	{"LANGUAGE", Query_for_list_of_languages},
	{"LARGE OBJECT", NULL, NULL, NULL, NULL, THING_NO_CREATE | THING_NO_DROP},
	{"MATERIALIZED VIEW", NULL, NULL, &Query_for_list_of_matviews},
	{"OPERATOR", NULL, NULL, NULL}, /* Querying for this is probably not such
									 * a good idea. */
	{"OR REPLACE", NULL, NULL, NULL, NULL, THING_NO_DROP | THING_NO_ALTER},
	{"OWNED", NULL, NULL, NULL, NULL, THING_NO_CREATE | THING_NO_ALTER},	/* for DROP OWNED BY ... */
	{"PARSER", NULL, NULL, &Query_for_list_of_ts_parsers, NULL, THING_NO_SHOW},
	{"POLICY", NULL, NULL, NULL},
	{"PROCEDURE", NULL, NULL, Query_for_list_of_procedures},
	{"PUBLICATION", NULL, Query_for_list_of_publications},
	{"ROLE", Query_for_list_of_roles},
	{"ROUTINE", NULL, NULL, &Query_for_list_of_routines, NULL, THING_NO_CREATE},
	{"RULE", "SELECT rulename FROM pg_catalog.pg_rules WHERE rulename LIKE '%s'"},
	{"SCHEMA", Query_for_list_of_schemas},
	{"SEQUENCE", NULL, NULL, &Query_for_list_of_sequences},
	{"SERVER", Query_for_list_of_servers},
	{"STATISTICS", NULL, NULL, &Query_for_list_of_statistics},
	{"SUBSCRIPTION", NULL, Query_for_list_of_subscriptions},
	{"SYSTEM", NULL, NULL, NULL, NULL, THING_NO_CREATE | THING_NO_DROP},
	{"TABLE", NULL, NULL, &Query_for_list_of_tables},
	{"TABLESPACE", Query_for_list_of_tablespaces},
	{"TEMP", NULL, NULL, NULL, NULL, THING_NO_DROP | THING_NO_ALTER},	/* for CREATE TEMP TABLE
																		 * ... */
	{"TEMPLATE", NULL, NULL, &Query_for_list_of_ts_templates, NULL, THING_NO_SHOW},
	{"TEMPORARY", NULL, NULL, NULL, NULL, THING_NO_DROP | THING_NO_ALTER},	/* for CREATE TEMPORARY
																			 * TABLE ... */
	{"TEXT SEARCH", NULL, NULL, NULL},
	{"TRANSFORM", NULL, NULL, NULL, NULL, THING_NO_ALTER},
	{"TRIGGER", "SELECT tgname FROM pg_catalog.pg_trigger WHERE tgname LIKE '%s' AND NOT tgisinternal"},
	{"TYPE", NULL, NULL, &Query_for_list_of_datatypes},
	{"UNIQUE", NULL, NULL, NULL, NULL, THING_NO_DROP | THING_NO_ALTER}, /* for CREATE UNIQUE
																		 * INDEX ... */
	{"UNLOGGED", NULL, NULL, NULL, NULL, THING_NO_DROP | THING_NO_ALTER},	/* for CREATE UNLOGGED
																			 * TABLE ... */
	{"USER", Query_for_list_of_roles, NULL, NULL, Keywords_for_user_thing},
	{"USER MAPPING FOR", NULL, NULL, NULL},
	{"VIEW", NULL, NULL, &Query_for_list_of_views},
	{NULL}						/* end of list */
};

/* Storage parameters for CREATE TABLE and ALTER TABLE */
static const char *const table_storage_parameters[] = {
	"autovacuum_analyze_scale_factor",
	"autovacuum_analyze_threshold",
	"autovacuum_enabled",
	"autovacuum_freeze_max_age",
	"autovacuum_freeze_min_age",
	"autovacuum_freeze_table_age",
	"autovacuum_multixact_freeze_max_age",
	"autovacuum_multixact_freeze_min_age",
	"autovacuum_multixact_freeze_table_age",
	"autovacuum_vacuum_cost_delay",
	"autovacuum_vacuum_cost_limit",
	"autovacuum_vacuum_insert_scale_factor",
	"autovacuum_vacuum_insert_threshold",
	"autovacuum_vacuum_scale_factor",
	"autovacuum_vacuum_threshold",
	"fillfactor",
	"log_autovacuum_min_duration",
	"parallel_workers",
	"toast.autovacuum_enabled",
	"toast.autovacuum_freeze_max_age",
	"toast.autovacuum_freeze_min_age",
	"toast.autovacuum_freeze_table_age",
	"toast.autovacuum_multixact_freeze_max_age",
	"toast.autovacuum_multixact_freeze_min_age",
	"toast.autovacuum_multixact_freeze_table_age",
	"toast.autovacuum_vacuum_cost_delay",
	"toast.autovacuum_vacuum_cost_limit",
	"toast.autovacuum_vacuum_insert_scale_factor",
	"toast.autovacuum_vacuum_insert_threshold",
	"toast.autovacuum_vacuum_scale_factor",
	"toast.autovacuum_vacuum_threshold",
	"toast.log_autovacuum_min_duration",
	"toast.vacuum_index_cleanup",
	"toast.vacuum_truncate",
	"toast_tuple_target",
	"user_catalog_table",
	"vacuum_index_cleanup",
	"vacuum_truncate",
	NULL
};

/* Optional parameters for CREATE VIEW and ALTER VIEW */
static const char *const view_optional_parameters[] = {
	"check_option",
	"security_barrier",
	"security_invoker",
	NULL
};

/* Forward declaration of functions */
static char **psql_completion(const char *text, int start, int end);
static char *create_command_generator(const char *text, int state);
static char *drop_command_generator(const char *text, int state);
static char *alter_command_generator(const char *text, int state);
static char *complete_from_query(const char *text, int state);
static char *complete_from_versioned_query(const char *text, int state);
static char *complete_from_schema_query(const char *text, int state);
static char *complete_from_versioned_schema_query(const char *text, int state);
static char *_complete_from_query(const char *simple_query,
								  const SchemaQuery *schema_query,
								  const char *const *keywords,
								  bool verbatim,
								  const char *text, int state);
static void set_completion_reference(const char *word);
static void set_completion_reference_verbatim(const char *word);
static char *complete_from_list(const char *text, int state);
static char *complete_from_const(const char *text, int state);
static void append_variable_names(char ***varnames, int *nvars,
								  int *maxvars, const char *varname,
								  const char *prefix, const char *suffix);
static char **complete_from_variables(const char *text,
									  const char *prefix, const char *suffix, bool need_value);
static char *complete_from_files(const char *text, int state);

static char *pg_strdup_keyword_case(const char *s, const char *ref);
static char *escape_string(const char *text);
static char *make_like_pattern(const char *word);
static void parse_identifier(const char *ident,
							 char **schemaname, char **objectname,
							 bool *schemaquoted, bool *objectquoted);
static char *requote_identifier(const char *schemaname, const char *objectname,
								bool quote_schema, bool quote_object);
static bool identifier_needs_quotes(const char *ident);
static PGresult *exec_query(const char *query);

static char **get_previous_words(int point, char **buffer, int *nwords);

static char *get_guctype(const char *varname);

#ifdef USE_FILENAME_QUOTING_FUNCTIONS
static char *quote_file_name(char *fname, int match_type, char *quote_pointer);
static char *dequote_file_name(char *fname, int quote_char);
#endif


/*
 * Initialize the readline library for our purposes.
 */
void
initialize_readline(void)
{
	rl_readline_name = (char *) pset.progname;
	rl_attempted_completion_function = psql_completion;

#ifdef USE_FILENAME_QUOTING_FUNCTIONS
	rl_filename_quoting_function = quote_file_name;
	rl_filename_dequoting_function = dequote_file_name;
#endif

	rl_basic_word_break_characters = WORD_BREAKS;

	/*
	 * Ideally we'd include '"' in rl_completer_quote_characters too, which
	 * should allow us to complete quoted identifiers that include spaces.
	 * However, the library support for rl_completer_quote_characters is
	 * presently too inconsistent to want to mess with that.  (Note in
	 * particular that libedit has this variable but completely ignores it.)
	 */
	rl_completer_quote_characters = "'";

	/*
	 * Set rl_filename_quote_characters to "all possible characters",
	 * otherwise Readline will skip filename quoting if it thinks a filename
	 * doesn't need quoting.  Readline actually interprets this as bytes, so
	 * there are no encoding considerations here.
	 */
#ifdef HAVE_RL_FILENAME_QUOTE_CHARACTERS
	{
		unsigned char *fqc = (unsigned char *) pg_malloc(256);

		for (int i = 0; i < 255; i++)
			fqc[i] = (unsigned char) (i + 1);
		fqc[255] = '\0';
		rl_filename_quote_characters = (const char *) fqc;
	}
#endif

	completion_max_records = 1000;

	/*
	 * There is a variable rl_completion_query_items for this but apparently
	 * it's not defined everywhere.
	 */
}

/*
 * Check if 'word' matches any of the '|'-separated strings in 'pattern',
 * using case-insensitive or case-sensitive comparisons.
 *
 * If pattern is NULL, it's a wild card that matches any word.
 * If pattern begins with '!', the result is negated, ie we check that 'word'
 * does *not* match any alternative appearing in the rest of 'pattern'.
 * Any alternative can contain '*' which is a wild card, i.e., it can match
 * any substring; however, we allow at most one '*' per alternative.
 *
 * For readability, callers should use the macros MatchAny and MatchAnyExcept
 * to invoke those two special cases for 'pattern'.  (But '|' and '*' must
 * just be written directly in patterns.)
 */
#define MatchAny  NULL
#define MatchAnyExcept(pattern)  ("!" pattern)

static bool
word_matches(const char *pattern,
			 const char *word,
			 bool case_sensitive)
{
	size_t		wordlen;

#define cimatch(s1, s2, n) \
	(case_sensitive ? strncmp(s1, s2, n) == 0 : pg_strncasecmp(s1, s2, n) == 0)

	/* NULL pattern matches anything. */
	if (pattern == NULL)
		return true;

	/* Handle negated patterns from the MatchAnyExcept macro. */
	if (*pattern == '!')
		return !word_matches(pattern + 1, word, case_sensitive);

	/* Else consider each alternative in the pattern. */
	wordlen = strlen(word);
	for (;;)
	{
		const char *star = NULL;
		const char *c;

		/* Find end of current alternative, and locate any wild card. */
		c = pattern;
		while (*c != '\0' && *c != '|')
		{
			if (*c == '*')
				star = c;
			c++;
		}
		/* Was there a wild card? */
		if (star)
		{
			/* Yes, wildcard match? */
			size_t		beforelen = star - pattern,
						afterlen = c - star - 1;

			if (wordlen >= (beforelen + afterlen) &&
				cimatch(word, pattern, beforelen) &&
				cimatch(word + wordlen - afterlen, star + 1, afterlen))
				return true;
		}
		else
		{
			/* No, plain match? */
			if (wordlen == (c - pattern) &&
				cimatch(word, pattern, wordlen))
				return true;
		}
		/* Out of alternatives? */
		if (*c == '\0')
			break;
		/* Nope, try next alternative. */
		pattern = c + 1;
	}

	return false;
}

/*
 * Implementation of TailMatches and TailMatchesCS macros: do the last N words
 * in previous_words match the variadic arguments?
 *
 * The array indexing might look backwards, but remember that
 * previous_words[0] contains the *last* word on the line, not the first.
 */
static bool
TailMatchesImpl(bool case_sensitive,
				int previous_words_count, char **previous_words,
				int narg,...)
{
	va_list		args;

	if (previous_words_count < narg)
		return false;

	va_start(args, narg);

	for (int argno = 0; argno < narg; argno++)
	{
		const char *arg = va_arg(args, const char *);

		if (!word_matches(arg, previous_words[narg - argno - 1],
						  case_sensitive))
		{
			va_end(args);
			return false;
		}
	}

	va_end(args);

	return true;
}

/*
 * Implementation of Matches and MatchesCS macros: do all of the words
 * in previous_words match the variadic arguments?
 */
static bool
MatchesImpl(bool case_sensitive,
			int previous_words_count, char **previous_words,
			int narg,...)
{
	va_list		args;

	if (previous_words_count != narg)
		return false;

	va_start(args, narg);

	for (int argno = 0; argno < narg; argno++)
	{
		const char *arg = va_arg(args, const char *);

		if (!word_matches(arg, previous_words[narg - argno - 1],
						  case_sensitive))
		{
			va_end(args);
			return false;
		}
	}

	va_end(args);

	return true;
}

/*
 * Implementation of HeadMatches and HeadMatchesCS macros: do the first N
 * words in previous_words match the variadic arguments?
 */
static bool
HeadMatchesImpl(bool case_sensitive,
				int previous_words_count, char **previous_words,
				int narg,...)
{
	va_list		args;

	if (previous_words_count < narg)
		return false;

	va_start(args, narg);

	for (int argno = 0; argno < narg; argno++)
	{
		const char *arg = va_arg(args, const char *);

		if (!word_matches(arg, previous_words[previous_words_count - argno - 1],
						  case_sensitive))
		{
			va_end(args);
			return false;
		}
	}

	va_end(args);

	return true;
}

/*
 * Check if the final character of 's' is 'c'.
 */
static bool
ends_with(const char *s, char c)
{
	size_t		length = strlen(s);

	return (length > 0 && s[length - 1] == c);
}

/*
 * The completion function.
 *
 * According to readline spec this gets passed the text entered so far and its
 * start and end positions in the readline buffer. The return value is some
 * partially obscure list format that can be generated by readline's
 * rl_completion_matches() function, so we don't have to worry about it.
 */
static char **
psql_completion(const char *text, int start, int end)
{
	/* This is the variable we'll return. */
	char	  **matches = NULL;

	/* Workspace for parsed words. */
	char	   *words_buffer;

	/* This array will contain pointers to parsed words. */
	char	  **previous_words;

	/* The number of words found on the input line. */
	int			previous_words_count;

	/*
	 * For compactness, we use these macros to reference previous_words[].
	 * Caution: do not access a previous_words[] entry without having checked
	 * previous_words_count to be sure it's valid.  In most cases below, that
	 * check is implicit in a TailMatches() or similar macro, but in some
	 * places we have to check it explicitly.
	 */
#define prev_wd   (previous_words[0])
#define prev2_wd  (previous_words[1])
#define prev3_wd  (previous_words[2])
#define prev4_wd  (previous_words[3])
#define prev5_wd  (previous_words[4])
#define prev6_wd  (previous_words[5])
#define prev7_wd  (previous_words[6])
#define prev8_wd  (previous_words[7])
#define prev9_wd  (previous_words[8])

	/* Match the last N words before point, case-insensitively. */
#define TailMatches(...) \
	TailMatchesImpl(false, previous_words_count, previous_words, \
					VA_ARGS_NARGS(__VA_ARGS__), __VA_ARGS__)

	/* Match the last N words before point, case-sensitively. */
#define TailMatchesCS(...) \
	TailMatchesImpl(true, previous_words_count, previous_words, \
					VA_ARGS_NARGS(__VA_ARGS__), __VA_ARGS__)

	/* Match N words representing all of the line, case-insensitively. */
#define Matches(...) \
	MatchesImpl(false, previous_words_count, previous_words, \
				VA_ARGS_NARGS(__VA_ARGS__), __VA_ARGS__)

	/* Match N words representing all of the line, case-sensitively. */
#define MatchesCS(...) \
	MatchesImpl(true, previous_words_count, previous_words, \
				VA_ARGS_NARGS(__VA_ARGS__), __VA_ARGS__)

	/* Match the first N words on the line, case-insensitively. */
#define HeadMatches(...) \
	HeadMatchesImpl(false, previous_words_count, previous_words, \
					VA_ARGS_NARGS(__VA_ARGS__), __VA_ARGS__)

	/* Match the first N words on the line, case-sensitively. */
#define HeadMatchesCS(...) \
	HeadMatchesImpl(true, previous_words_count, previous_words, \
					VA_ARGS_NARGS(__VA_ARGS__), __VA_ARGS__)

	/* Known command-starting keywords. */
	static const char *const sql_commands[] = {
		"ABORT", "ALTER", "ANALYZE", "BEGIN", "CALL", "CHECKPOINT", "CLOSE", "CLUSTER",
		"COMMENT", "COMMIT", "COPY", "CREATE", "DEALLOCATE", "DECLARE",
		"DELETE FROM", "DISCARD", "DO", "DROP", "END", "EXECUTE", "EXPLAIN",
		"FETCH", "GRANT", "IMPORT FOREIGN SCHEMA", "INSERT INTO", "LISTEN", "LOAD", "LOCK",
		"MERGE INTO", "MOVE", "NOTIFY", "PREPARE",
		"REASSIGN", "REFRESH MATERIALIZED VIEW", "REINDEX", "RELEASE",
		"RESET", "REVOKE", "ROLLBACK",
		"SAVEPOINT", "SECURITY LABEL", "SELECT", "SET", "SHOW", "START",
		"TABLE", "TRUNCATE", "UNLISTEN", "UPDATE", "VACUUM", "VALUES", "WITH",
		NULL
	};

	/* psql's backslash commands. */
	static const char *const backslash_commands[] = {
		"\\a",
		"\\bind", "\\bind_named",
		"\\connect", "\\conninfo", "\\C", "\\cd", "\\close", "\\copy",
		"\\copyright", "\\crosstabview",
		"\\d", "\\da", "\\dA", "\\dAc", "\\dAf", "\\dAo", "\\dAp",
		"\\db", "\\dc", "\\dconfig", "\\dC", "\\dd", "\\ddp", "\\dD",
		"\\des", "\\det", "\\deu", "\\dew", "\\dE", "\\df",
		"\\dF", "\\dFd", "\\dFp", "\\dFt", "\\dg", "\\di", "\\dl", "\\dL",
		"\\dm", "\\dn", "\\do", "\\dO", "\\dp", "\\dP", "\\dPi", "\\dPt",
		"\\drds", "\\drg", "\\dRs", "\\dRp", "\\ds",
		"\\dt", "\\dT", "\\dv", "\\du", "\\dx", "\\dX", "\\dy",
		"\\echo", "\\edit", "\\ef", "\\elif", "\\else", "\\encoding",
		"\\endif", "\\errverbose", "\\ev",
		"\\f",
		"\\g", "\\gdesc", "\\getenv", "\\gexec", "\\gset", "\\gx",
		"\\help", "\\html",
		"\\if", "\\include", "\\include_relative", "\\ir",
		"\\list", "\\lo_import", "\\lo_export", "\\lo_list", "\\lo_unlink",
		"\\out",
		"\\parse", "\\password", "\\print", "\\prompt", "\\pset",
		"\\qecho", "\\quit",
		"\\reset",
		"\\s", "\\set", "\\setenv", "\\sf", "\\sv",
		"\\t", "\\T", "\\timing",
		"\\unset",
		"\\x",
		"\\warn", "\\watch", "\\write",
		"\\z",
		"\\!", "\\?",
		NULL
	};

	/*
	 * Temporary workaround for a bug in recent (2019) libedit: it incorrectly
	 * de-escapes the input "text", causing us to fail to recognize backslash
	 * commands.  So get the string to look at from rl_line_buffer instead.
	 */
	char	   *text_copy = pnstrdup(rl_line_buffer + start, end - start);
	text = text_copy;

	/* Remember last char of the given input word. */
	completion_last_char = (end > start) ? text[end - start - 1] : '\0';

	/* We usually want the append character to be a space. */
	rl_completion_append_character = ' ';

	/* Clear a few things. */
	completion_charp = NULL;
	completion_charpp = NULL;
	completion_vquery = NULL;
	completion_squery = NULL;
	completion_ref_object = NULL;
	completion_ref_schema = NULL;

	/*
	 * Scan the input line to extract the words before our current position.
	 * According to those we'll make some smart decisions on what the user is
	 * probably intending to type.
	 */
	previous_words = get_previous_words(start,
										&words_buffer,
										&previous_words_count);

	/* If current word is a backslash command, offer completions for that */
	if (text[0] == '\\')
		COMPLETE_WITH_LIST_CS(backslash_commands);

	/* If current word is a variable interpolation, handle that case */
	else if (text[0] == ':' && text[1] != ':')
	{
		if (text[1] == '\'')
			matches = complete_from_variables(text, ":'", "'", true);
		else if (text[1] == '"')
			matches = complete_from_variables(text, ":\"", "\"", true);
		else if (text[1] == '{' && text[2] == '?')
			matches = complete_from_variables(text, ":{?", "}", true);
		else
			matches = complete_from_variables(text, ":", "", true);
	}

	/* If no previous word, suggest one of the basic sql commands */
	else if (previous_words_count == 0)
		COMPLETE_WITH_LIST(sql_commands);

/* CREATE */
	/* complete with something you can create */
	else if (TailMatches("CREATE"))
	{
		/* only some object types can be created as part of CREATE SCHEMA */
		if (HeadMatches("CREATE", "SCHEMA"))
			COMPLETE_WITH("TABLE", "VIEW", "INDEX", "SEQUENCE", "TRIGGER",
			/* for INDEX and TABLE/SEQUENCE, respectively */
						  "UNIQUE", "UNLOGGED");
		else
			matches = rl_completion_matches(text, create_command_generator);
	}
	/* complete with something you can create or replace */
	else if (TailMatches("CREATE", "OR", "REPLACE"))
		COMPLETE_WITH("FUNCTION", "PROCEDURE", "LANGUAGE", "RULE", "VIEW",
					  "AGGREGATE", "TRANSFORM", "TRIGGER");

/* DROP, but not DROP embedded in other commands */
	/* complete with something you can drop */
	else if (Matches("DROP"))
		matches = rl_completion_matches(text, drop_command_generator);

/* ALTER */

	/* ALTER TABLE */
	else if (Matches("ALTER", "TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_tables,
										"ALL IN TABLESPACE");

	/* ALTER something */
	else if (Matches("ALTER"))
		matches = rl_completion_matches(text, alter_command_generator);
	/* ALTER TABLE,INDEX,MATERIALIZED VIEW ALL IN TABLESPACE xxx */
	else if (TailMatches("ALL", "IN", "TABLESPACE", MatchAny))
		COMPLETE_WITH("SET TABLESPACE", "OWNED BY");
	/* ALTER TABLE,INDEX,MATERIALIZED VIEW ALL IN TABLESPACE xxx OWNED BY */
	else if (TailMatches("ALL", "IN", "TABLESPACE", MatchAny, "OWNED", "BY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	/* ALTER TABLE,INDEX,MATERIALIZED VIEW ALL IN TABLESPACE xxx OWNED BY xxx */
	else if (TailMatches("ALL", "IN", "TABLESPACE", MatchAny, "OWNED", "BY", MatchAny))
		COMPLETE_WITH("SET TABLESPACE");
	/* ALTER AGGREGATE,FUNCTION,PROCEDURE,ROUTINE <name> */
	else if (Matches("ALTER", "AGGREGATE|FUNCTION|PROCEDURE|ROUTINE", MatchAny))
		COMPLETE_WITH("(");
	/* ALTER AGGREGATE <name> (...) */
	else if (Matches("ALTER", "AGGREGATE", MatchAny, MatchAny))
	{
		if (ends_with(prev_wd, ')'))
			COMPLETE_WITH("OWNER TO", "RENAME TO", "SET SCHEMA");
		else
			COMPLETE_WITH_FUNCTION_ARG(prev2_wd);
	}
	/* ALTER FUNCTION <name> (...) */
	else if (Matches("ALTER", "FUNCTION", MatchAny, MatchAny))
	{
		if (ends_with(prev_wd, ')'))
			COMPLETE_WITH(Alter_function_options);
		else
			COMPLETE_WITH_FUNCTION_ARG(prev2_wd);
	}
	/* ALTER PROCEDURE <name> (...) */
	else if (Matches("ALTER", "PROCEDURE", MatchAny, MatchAny))
	{
		if (ends_with(prev_wd, ')'))
			COMPLETE_WITH(Alter_procedure_options);
		else
			COMPLETE_WITH_FUNCTION_ARG(prev2_wd);
	}
	/* ALTER ROUTINE <name> (...) */
	else if (Matches("ALTER", "ROUTINE", MatchAny, MatchAny))
	{
		if (ends_with(prev_wd, ')'))
			COMPLETE_WITH(Alter_routine_options);
		else
			COMPLETE_WITH_FUNCTION_ARG(prev2_wd);
	}
	/* ALTER FUNCTION|ROUTINE <name> (...) PARALLEL */
	else if (Matches("ALTER", "FUNCTION|ROUTINE", MatchAny, MatchAny, "PARALLEL"))
		COMPLETE_WITH("RESTRICTED", "SAFE", "UNSAFE");
	/* ALTER FUNCTION|PROCEDURE|ROUTINE <name> (...) [EXTERNAL] SECURITY */
	else if (Matches("ALTER", "FUNCTION|PROCEDURE|ROUTINE", MatchAny, MatchAny, "SECURITY") ||
			 Matches("ALTER", "FUNCTION|PROCEDURE|ROUTINE", MatchAny, MatchAny, "EXTERNAL", "SECURITY"))
		COMPLETE_WITH("DEFINER", "INVOKER");
	/* ALTER FUNCTION|PROCEDURE|ROUTINE <name> (...) RESET */
	else if (Matches("ALTER", "FUNCTION|PROCEDURE|ROUTINE", MatchAny, MatchAny, "RESET"))
		COMPLETE_WITH_QUERY_VERBATIM_PLUS(Query_for_list_of_set_vars,
										  "ALL");
	/* ALTER FUNCTION|PROCEDURE|ROUTINE <name> (...) SET */
	else if (Matches("ALTER", "FUNCTION|PROCEDURE|ROUTINE", MatchAny, MatchAny, "SET"))
		COMPLETE_WITH_QUERY_VERBATIM_PLUS(Query_for_list_of_set_vars,
										  "SCHEMA");

	/* ALTER PUBLICATION <name> */
	else if (Matches("ALTER", "PUBLICATION", MatchAny))
		COMPLETE_WITH("ADD", "DROP", "OWNER TO", "RENAME TO", "SET");
	/* ALTER PUBLICATION <name> ADD */
	else if (Matches("ALTER", "PUBLICATION", MatchAny, "ADD"))
		COMPLETE_WITH("TABLES IN SCHEMA", "TABLE");
	else if (Matches("ALTER", "PUBLICATION", MatchAny, "ADD|SET", "TABLE") ||
			 (HeadMatches("ALTER", "PUBLICATION", MatchAny, "ADD|SET", "TABLE") &&
			  ends_with(prev_wd, ',')))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

	/*
	 * "ALTER PUBLICATION <name> SET TABLE <name> WHERE (" - complete with
	 * table attributes
	 *
	 * "ALTER PUBLICATION <name> ADD TABLE <name> WHERE (" - complete with
	 * table attributes
	 */
	else if (HeadMatches("ALTER", "PUBLICATION", MatchAny) && TailMatches("WHERE"))
		COMPLETE_WITH("(");
	else if (HeadMatches("ALTER", "PUBLICATION", MatchAny) && TailMatches("WHERE", "("))
		COMPLETE_WITH_ATTR(prev3_wd);
	else if (HeadMatches("ALTER", "PUBLICATION", MatchAny, "ADD|SET", "TABLE") &&
			 !TailMatches("WHERE", "(*)"))
		COMPLETE_WITH(",", "WHERE (");
	else if (HeadMatches("ALTER", "PUBLICATION", MatchAny, "ADD|SET", "TABLE"))
		COMPLETE_WITH(",");
	/* ALTER PUBLICATION <name> DROP */
	else if (Matches("ALTER", "PUBLICATION", MatchAny, "DROP"))
		COMPLETE_WITH("TABLES IN SCHEMA", "TABLE");
	/* ALTER PUBLICATION <name> SET */
	else if (Matches("ALTER", "PUBLICATION", MatchAny, "SET"))
		COMPLETE_WITH("(", "TABLES IN SCHEMA", "TABLE");
	else if (Matches("ALTER", "PUBLICATION", MatchAny, "ADD|DROP|SET", "TABLES", "IN", "SCHEMA"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_schemas
								 " AND nspname NOT LIKE E'pg\\\\_%%'",
								 "CURRENT_SCHEMA");
	/* ALTER PUBLICATION <name> SET ( */
	else if (HeadMatches("ALTER", "PUBLICATION", MatchAny) && TailMatches("SET", "("))
		COMPLETE_WITH("publish", "publish_via_partition_root");
	/* ALTER SUBSCRIPTION <name> */
	else if (Matches("ALTER", "SUBSCRIPTION", MatchAny))
		COMPLETE_WITH("CONNECTION", "ENABLE", "DISABLE", "OWNER TO",
					  "RENAME TO", "REFRESH PUBLICATION", "SET", "SKIP (",
					  "ADD PUBLICATION", "DROP PUBLICATION");
	/* ALTER SUBSCRIPTION <name> REFRESH PUBLICATION */
	else if (HeadMatches("ALTER", "SUBSCRIPTION", MatchAny) &&
			 TailMatches("REFRESH", "PUBLICATION"))
		COMPLETE_WITH("WITH (");
	/* ALTER SUBSCRIPTION <name> REFRESH PUBLICATION WITH ( */
	else if (HeadMatches("ALTER", "SUBSCRIPTION", MatchAny) &&
			 TailMatches("REFRESH", "PUBLICATION", "WITH", "("))
		COMPLETE_WITH("copy_data");
	/* ALTER SUBSCRIPTION <name> SET */
	else if (Matches("ALTER", "SUBSCRIPTION", MatchAny, "SET"))
		COMPLETE_WITH("(", "PUBLICATION");
	/* ALTER SUBSCRIPTION <name> SET ( */
	else if (HeadMatches("ALTER", "SUBSCRIPTION", MatchAny) && TailMatches("SET", "("))
		COMPLETE_WITH("binary", "disable_on_error", "failover", "origin",
					  "password_required", "run_as_owner", "slot_name",
					  "streaming", "synchronous_commit", "two_phase");
	/* ALTER SUBSCRIPTION <name> SKIP ( */
	else if (HeadMatches("ALTER", "SUBSCRIPTION", MatchAny) && TailMatches("SKIP", "("))
		COMPLETE_WITH("lsn");
	/* ALTER SUBSCRIPTION <name> SET PUBLICATION */
	else if (HeadMatches("ALTER", "SUBSCRIPTION", MatchAny) && TailMatches("SET", "PUBLICATION"))
	{
		/* complete with nothing here as this refers to remote publications */
	}
	/* ALTER SUBSCRIPTION <name> ADD|DROP|SET PUBLICATION <name> */
	else if (HeadMatches("ALTER", "SUBSCRIPTION", MatchAny) &&
			 TailMatches("ADD|DROP|SET", "PUBLICATION", MatchAny))
		COMPLETE_WITH("WITH (");
	/* ALTER SUBSCRIPTION <name> ADD|DROP|SET PUBLICATION <name> WITH ( */
	else if (HeadMatches("ALTER", "SUBSCRIPTION", MatchAny) &&
			 TailMatches("ADD|DROP|SET", "PUBLICATION", MatchAny, "WITH", "("))
		COMPLETE_WITH("copy_data", "refresh");

	/* ALTER SCHEMA <name> */
	else if (Matches("ALTER", "SCHEMA", MatchAny))
		COMPLETE_WITH("OWNER TO", "RENAME TO");

	/* ALTER COLLATION <name> */
	else if (Matches("ALTER", "COLLATION", MatchAny))
		COMPLETE_WITH("OWNER TO", "REFRESH VERSION", "RENAME TO", "SET SCHEMA");

	/* ALTER CONVERSION <name> */
	else if (Matches("ALTER", "CONVERSION", MatchAny))
		COMPLETE_WITH("OWNER TO", "RENAME TO", "SET SCHEMA");

	/* ALTER DATABASE <name> */
	else if (Matches("ALTER", "DATABASE", MatchAny))
		COMPLETE_WITH("RESET", "SET", "OWNER TO", "REFRESH COLLATION VERSION", "RENAME TO",
					  "IS_TEMPLATE", "ALLOW_CONNECTIONS",
					  "CONNECTION LIMIT");

	/* ALTER DATABASE <name> SET TABLESPACE */
	else if (Matches("ALTER", "DATABASE", MatchAny, "SET", "TABLESPACE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);

	/* ALTER EVENT TRIGGER */
	else if (Matches("ALTER", "EVENT", "TRIGGER"))
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);

	/* ALTER EVENT TRIGGER <name> */
	else if (Matches("ALTER", "EVENT", "TRIGGER", MatchAny))
		COMPLETE_WITH("DISABLE", "ENABLE", "OWNER TO", "RENAME TO");

	/* ALTER EVENT TRIGGER <name> ENABLE */
	else if (Matches("ALTER", "EVENT", "TRIGGER", MatchAny, "ENABLE"))
		COMPLETE_WITH("REPLICA", "ALWAYS");

	/* ALTER EXTENSION <name> */
	else if (Matches("ALTER", "EXTENSION", MatchAny))
		COMPLETE_WITH("ADD", "DROP", "UPDATE", "SET SCHEMA");

	/* ALTER EXTENSION <name> ADD|DROP */
	else if (Matches("ALTER", "EXTENSION", MatchAny, "ADD|DROP"))
		COMPLETE_WITH("ACCESS METHOD", "AGGREGATE", "CAST", "COLLATION",
					  "CONVERSION", "DOMAIN", "EVENT TRIGGER", "FOREIGN",
					  "FUNCTION", "MATERIALIZED VIEW", "OPERATOR",
					  "LANGUAGE", "PROCEDURE", "ROUTINE", "SCHEMA",
					  "SEQUENCE", "SERVER", "TABLE", "TEXT SEARCH",
					  "TRANSFORM FOR", "TYPE", "VIEW");

	/* ALTER EXTENSION <name> ADD|DROP FOREIGN */
	else if (Matches("ALTER", "EXTENSION", MatchAny, "ADD|DROP", "FOREIGN"))
		COMPLETE_WITH("DATA WRAPPER", "TABLE");

	/* ALTER EXTENSION <name> ADD|DROP OPERATOR */
	else if (Matches("ALTER", "EXTENSION", MatchAny, "ADD|DROP", "OPERATOR"))
		COMPLETE_WITH("CLASS", "FAMILY");

	/* ALTER EXTENSION <name> ADD|DROP TEXT SEARCH */
	else if (Matches("ALTER", "EXTENSION", MatchAny, "ADD|DROP", "TEXT", "SEARCH"))
		COMPLETE_WITH("CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE");

	/* ALTER EXTENSION <name> UPDATE */
	else if (Matches("ALTER", "EXTENSION", MatchAny, "UPDATE"))
		COMPLETE_WITH("TO");

	/* ALTER EXTENSION <name> UPDATE TO */
	else if (Matches("ALTER", "EXTENSION", MatchAny, "UPDATE", "TO"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_QUERY(Query_for_list_of_available_extension_versions);
	}

	/* ALTER FOREIGN */
	else if (Matches("ALTER", "FOREIGN"))
		COMPLETE_WITH("DATA WRAPPER", "TABLE");

	/* ALTER FOREIGN DATA WRAPPER <name> */
	else if (Matches("ALTER", "FOREIGN", "DATA", "WRAPPER", MatchAny))
		COMPLETE_WITH("HANDLER", "VALIDATOR", "NO",
					  "OPTIONS", "OWNER TO", "RENAME TO");
	else if (Matches("ALTER", "FOREIGN", "DATA", "WRAPPER", MatchAny, "NO"))
		COMPLETE_WITH("HANDLER", "VALIDATOR");

	/* ALTER FOREIGN TABLE <name> */
	else if (Matches("ALTER", "FOREIGN", "TABLE", MatchAny))
		COMPLETE_WITH("ADD", "ALTER", "DISABLE TRIGGER", "DROP", "ENABLE",
					  "INHERIT", "NO INHERIT", "OPTIONS", "OWNER TO",
					  "RENAME", "SET", "VALIDATE CONSTRAINT");

	/* ALTER INDEX */
	else if (Matches("ALTER", "INDEX"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_indexes,
										"ALL IN TABLESPACE");
	/* ALTER INDEX <name> */
	else if (Matches("ALTER", "INDEX", MatchAny))
		COMPLETE_WITH("ALTER COLUMN", "OWNER TO", "RENAME TO", "SET",
					  "RESET", "ATTACH PARTITION",
					  "DEPENDS ON EXTENSION", "NO DEPENDS ON EXTENSION");
	else if (Matches("ALTER", "INDEX", MatchAny, "ATTACH"))
		COMPLETE_WITH("PARTITION");
	else if (Matches("ALTER", "INDEX", MatchAny, "ATTACH", "PARTITION"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes);
	/* ALTER INDEX <name> ALTER */
	else if (Matches("ALTER", "INDEX", MatchAny, "ALTER"))
		COMPLETE_WITH("COLUMN");
	/* ALTER INDEX <name> ALTER COLUMN */
	else if (Matches("ALTER", "INDEX", MatchAny, "ALTER", "COLUMN"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_SCHEMA_QUERY_VERBATIM(Query_for_list_of_attribute_numbers);
	}
	/* ALTER INDEX <name> ALTER COLUMN <colnum> */
	else if (Matches("ALTER", "INDEX", MatchAny, "ALTER", "COLUMN", MatchAny))
		COMPLETE_WITH("SET STATISTICS");
	/* ALTER INDEX <name> ALTER COLUMN <colnum> SET */
	else if (Matches("ALTER", "INDEX", MatchAny, "ALTER", "COLUMN", MatchAny, "SET"))
		COMPLETE_WITH("STATISTICS");
	/* ALTER INDEX <name> ALTER COLUMN <colnum> SET STATISTICS */
	else if (Matches("ALTER", "INDEX", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "STATISTICS"))
	{
		/* Enforce no completion here, as an integer has to be specified */
	}
	/* ALTER INDEX <name> SET */
	else if (Matches("ALTER", "INDEX", MatchAny, "SET"))
		COMPLETE_WITH("(", "TABLESPACE");
	/* ALTER INDEX <name> RESET */
	else if (Matches("ALTER", "INDEX", MatchAny, "RESET"))
		COMPLETE_WITH("(");
	/* ALTER INDEX <foo> SET|RESET ( */
	else if (Matches("ALTER", "INDEX", MatchAny, "RESET", "("))
		COMPLETE_WITH("fillfactor",
					  "deduplicate_items",	/* BTREE */
					  "fastupdate", "gin_pending_list_limit",	/* GIN */
					  "buffering",	/* GiST */
					  "pages_per_range", "autosummarize"	/* BRIN */
			);
	else if (Matches("ALTER", "INDEX", MatchAny, "SET", "("))
		COMPLETE_WITH("fillfactor =",
					  "deduplicate_items =",	/* BTREE */
					  "fastupdate =", "gin_pending_list_limit =",	/* GIN */
					  "buffering =",	/* GiST */
					  "pages_per_range =", "autosummarize ="	/* BRIN */
			);
	else if (Matches("ALTER", "INDEX", MatchAny, "NO", "DEPENDS"))
		COMPLETE_WITH("ON EXTENSION");
	else if (Matches("ALTER", "INDEX", MatchAny, "DEPENDS"))
		COMPLETE_WITH("ON EXTENSION");

	/* ALTER LANGUAGE <name> */
	else if (Matches("ALTER", "LANGUAGE", MatchAny))
		COMPLETE_WITH("OWNER TO", "RENAME TO");

	/* ALTER LARGE OBJECT <oid> */
	else if (Matches("ALTER", "LARGE", "OBJECT", MatchAny))
		COMPLETE_WITH("OWNER TO");

	/* ALTER MATERIALIZED VIEW */
	else if (Matches("ALTER", "MATERIALIZED", "VIEW"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_matviews,
										"ALL IN TABLESPACE");

	/* ALTER USER,ROLE <name> */
	else if (Matches("ALTER", "USER|ROLE", MatchAny) &&
			 !TailMatches("USER", "MAPPING"))
		COMPLETE_WITH("BYPASSRLS", "CONNECTION LIMIT", "CREATEDB", "CREATEROLE",
					  "ENCRYPTED PASSWORD", "INHERIT", "LOGIN", "NOBYPASSRLS",
					  "NOCREATEDB", "NOCREATEROLE", "NOINHERIT",
					  "NOLOGIN", "NOREPLICATION", "NOSUPERUSER", "PASSWORD",
					  "RENAME TO", "REPLICATION", "RESET", "SET", "SUPERUSER",
					  "VALID UNTIL", "WITH");

	/* ALTER USER,ROLE <name> WITH */
	else if (Matches("ALTER", "USER|ROLE", MatchAny, "WITH"))
		/* Similar to the above, but don't complete "WITH" again. */
		COMPLETE_WITH("BYPASSRLS", "CONNECTION LIMIT", "CREATEDB", "CREATEROLE",
					  "ENCRYPTED PASSWORD", "INHERIT", "LOGIN", "NOBYPASSRLS",
					  "NOCREATEDB", "NOCREATEROLE", "NOINHERIT",
					  "NOLOGIN", "NOREPLICATION", "NOSUPERUSER", "PASSWORD",
					  "RENAME TO", "REPLICATION", "RESET", "SET", "SUPERUSER",
					  "VALID UNTIL");

	/* ALTER DEFAULT PRIVILEGES */
	else if (Matches("ALTER", "DEFAULT", "PRIVILEGES"))
		COMPLETE_WITH("FOR", "GRANT", "IN SCHEMA", "REVOKE");
	/* ALTER DEFAULT PRIVILEGES FOR */
	else if (Matches("ALTER", "DEFAULT", "PRIVILEGES", "FOR"))
		COMPLETE_WITH("ROLE");
	/* ALTER DEFAULT PRIVILEGES IN */
	else if (Matches("ALTER", "DEFAULT", "PRIVILEGES", "IN"))
		COMPLETE_WITH("SCHEMA");
	/* ALTER DEFAULT PRIVILEGES FOR ROLE|USER ... */
	else if (Matches("ALTER", "DEFAULT", "PRIVILEGES", "FOR", "ROLE|USER",
					 MatchAny))
		COMPLETE_WITH("GRANT", "REVOKE", "IN SCHEMA");
	/* ALTER DEFAULT PRIVILEGES IN SCHEMA ... */
	else if (Matches("ALTER", "DEFAULT", "PRIVILEGES", "IN", "SCHEMA",
					 MatchAny))
		COMPLETE_WITH("GRANT", "REVOKE", "FOR ROLE");
	/* ALTER DEFAULT PRIVILEGES IN SCHEMA ... FOR */
	else if (Matches("ALTER", "DEFAULT", "PRIVILEGES", "IN", "SCHEMA",
					 MatchAny, "FOR"))
		COMPLETE_WITH("ROLE");
	/* ALTER DEFAULT PRIVILEGES FOR ROLE|USER ... IN SCHEMA ... */
	/* ALTER DEFAULT PRIVILEGES IN SCHEMA ... FOR ROLE|USER ... */
	else if (Matches("ALTER", "DEFAULT", "PRIVILEGES", "FOR", "ROLE|USER",
					 MatchAny, "IN", "SCHEMA", MatchAny) ||
			 Matches("ALTER", "DEFAULT", "PRIVILEGES", "IN", "SCHEMA",
					 MatchAny, "FOR", "ROLE|USER", MatchAny))
		COMPLETE_WITH("GRANT", "REVOKE");
	/* ALTER DOMAIN <name> */
	else if (Matches("ALTER", "DOMAIN", MatchAny))
		COMPLETE_WITH("ADD", "DROP", "OWNER TO", "RENAME", "SET",
					  "VALIDATE CONSTRAINT");
	/* ALTER DOMAIN <sth> DROP */
	else if (Matches("ALTER", "DOMAIN", MatchAny, "DROP"))
		COMPLETE_WITH("CONSTRAINT", "DEFAULT", "NOT NULL");
	/* ALTER DOMAIN <sth> DROP|RENAME|VALIDATE CONSTRAINT */
	else if (Matches("ALTER", "DOMAIN", MatchAny, "DROP|RENAME|VALIDATE", "CONSTRAINT"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_constraint_of_type);
	}
	/* ALTER DOMAIN <sth> RENAME */
	else if (Matches("ALTER", "DOMAIN", MatchAny, "RENAME"))
		COMPLETE_WITH("CONSTRAINT", "TO");
	/* ALTER DOMAIN <sth> RENAME CONSTRAINT <sth> */
	else if (Matches("ALTER", "DOMAIN", MatchAny, "RENAME", "CONSTRAINT", MatchAny))
		COMPLETE_WITH("TO");

	/* ALTER DOMAIN <sth> SET */
	else if (Matches("ALTER", "DOMAIN", MatchAny, "SET"))
		COMPLETE_WITH("DEFAULT", "NOT NULL", "SCHEMA");
	/* ALTER SEQUENCE <name> */
	else if (Matches("ALTER", "SEQUENCE", MatchAny))
		COMPLETE_WITH("AS", "INCREMENT", "MINVALUE", "MAXVALUE", "RESTART",
					  "START", "NO", "CACHE", "CYCLE", "SET", "OWNED BY",
					  "OWNER TO", "RENAME TO");
	/* ALTER SEQUENCE <name> AS */
	else if (TailMatches("ALTER", "SEQUENCE", MatchAny, "AS"))
		COMPLETE_WITH_CS("smallint", "integer", "bigint");
	/* ALTER SEQUENCE <name> NO */
	else if (Matches("ALTER", "SEQUENCE", MatchAny, "NO"))
		COMPLETE_WITH("MINVALUE", "MAXVALUE", "CYCLE");
	/* ALTER SEQUENCE <name> SET */
	else if (Matches("ALTER", "SEQUENCE", MatchAny, "SET"))
		COMPLETE_WITH("SCHEMA", "LOGGED", "UNLOGGED");
	/* ALTER SERVER <name> */
	else if (Matches("ALTER", "SERVER", MatchAny))
		COMPLETE_WITH("VERSION", "OPTIONS", "OWNER TO", "RENAME TO");
	/* ALTER SERVER <name> VERSION <version> */
	else if (Matches("ALTER", "SERVER", MatchAny, "VERSION", MatchAny))
		COMPLETE_WITH("OPTIONS");
	/* ALTER SYSTEM SET, RESET, RESET ALL */
	else if (Matches("ALTER", "SYSTEM"))
		COMPLETE_WITH("SET", "RESET");
	else if (Matches("ALTER", "SYSTEM", "SET|RESET"))
		COMPLETE_WITH_QUERY_VERBATIM_PLUS(Query_for_list_of_alter_system_set_vars,
										  "ALL");
	else if (Matches("ALTER", "SYSTEM", "SET", MatchAny))
		COMPLETE_WITH("TO");
	/* ALTER VIEW <name> */
	else if (Matches("ALTER", "VIEW", MatchAny))
		COMPLETE_WITH("ALTER COLUMN", "OWNER TO", "RENAME", "RESET", "SET");
	/* ALTER VIEW xxx RENAME */
	else if (Matches("ALTER", "VIEW", MatchAny, "RENAME"))
		COMPLETE_WITH_ATTR_PLUS(prev2_wd, "COLUMN", "TO");
	else if (Matches("ALTER", "VIEW", MatchAny, "ALTER|RENAME", "COLUMN"))
		COMPLETE_WITH_ATTR(prev3_wd);
	/* ALTER VIEW xxx ALTER [ COLUMN ] yyy */
	else if (Matches("ALTER", "VIEW", MatchAny, "ALTER", MatchAny) ||
			 Matches("ALTER", "VIEW", MatchAny, "ALTER", "COLUMN", MatchAny))
		COMPLETE_WITH("SET DEFAULT", "DROP DEFAULT");
	/* ALTER VIEW xxx RENAME yyy */
	else if (Matches("ALTER", "VIEW", MatchAny, "RENAME", MatchAnyExcept("TO")))
		COMPLETE_WITH("TO");
	/* ALTER VIEW xxx RENAME COLUMN yyy */
	else if (Matches("ALTER", "VIEW", MatchAny, "RENAME", "COLUMN", MatchAnyExcept("TO")))
		COMPLETE_WITH("TO");
	/* ALTER VIEW xxx RESET ( */
	else if (Matches("ALTER", "VIEW", MatchAny, "RESET"))
		COMPLETE_WITH("(");
	/* Complete ALTER VIEW xxx SET with "(" or "SCHEMA" */
	else if (Matches("ALTER", "VIEW", MatchAny, "SET"))
		COMPLETE_WITH("(", "SCHEMA");
	/* ALTER VIEW xxx SET|RESET ( yyy [= zzz] ) */
	else if (Matches("ALTER", "VIEW", MatchAny, "SET|RESET", "("))
		COMPLETE_WITH_LIST(view_optional_parameters);
	else if (Matches("ALTER", "VIEW", MatchAny, "SET", "(", MatchAny))
		COMPLETE_WITH("=");
	else if (Matches("ALTER", "VIEW", MatchAny, "SET", "(", "check_option", "="))
		COMPLETE_WITH("local", "cascaded");
	else if (Matches("ALTER", "VIEW", MatchAny, "SET", "(", "security_barrier|security_invoker", "="))
		COMPLETE_WITH("true", "false");

	/* ALTER MATERIALIZED VIEW <name> */
	else if (Matches("ALTER", "MATERIALIZED", "VIEW", MatchAny))
		COMPLETE_WITH("ALTER COLUMN", "CLUSTER ON", "DEPENDS ON EXTENSION",
					  "NO DEPENDS ON EXTENSION", "OWNER TO", "RENAME",
					  "RESET (", "SET");
	/* ALTER MATERIALIZED VIEW xxx RENAME */
	else if (Matches("ALTER", "MATERIALIZED", "VIEW", MatchAny, "RENAME"))
		COMPLETE_WITH_ATTR_PLUS(prev2_wd, "COLUMN", "TO");
	else if (Matches("ALTER", "MATERIALIZED", "VIEW", MatchAny, "ALTER|RENAME", "COLUMN"))
		COMPLETE_WITH_ATTR(prev3_wd);
	/* ALTER MATERIALIZED VIEW xxx RENAME yyy */
	else if (Matches("ALTER", "MATERIALIZED", "VIEW", MatchAny, "RENAME", MatchAnyExcept("TO")))
		COMPLETE_WITH("TO");
	/* ALTER MATERIALIZED VIEW xxx RENAME COLUMN yyy */
	else if (Matches("ALTER", "MATERIALIZED", "VIEW", MatchAny, "RENAME", "COLUMN", MatchAnyExcept("TO")))
		COMPLETE_WITH("TO");
	/* ALTER MATERIALIZED VIEW xxx SET */
	else if (Matches("ALTER", "MATERIALIZED", "VIEW", MatchAny, "SET"))
		COMPLETE_WITH("(", "ACCESS METHOD", "SCHEMA", "TABLESPACE", "WITHOUT CLUSTER");
	/* ALTER MATERIALIZED VIEW xxx SET ACCESS METHOD */
	else if (Matches("ALTER", "MATERIALIZED", "VIEW", MatchAny, "SET", "ACCESS", "METHOD"))
		COMPLETE_WITH_QUERY(Query_for_list_of_table_access_methods);

	/* ALTER POLICY <name> */
	else if (Matches("ALTER", "POLICY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_policies);
	/* ALTER POLICY <name> ON */
	else if (Matches("ALTER", "POLICY", MatchAny))
		COMPLETE_WITH("ON");
	/* ALTER POLICY <name> ON <table> */
	else if (Matches("ALTER", "POLICY", MatchAny, "ON"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables_for_policy);
	}
	/* ALTER POLICY <name> ON <table> - show options */
	else if (Matches("ALTER", "POLICY", MatchAny, "ON", MatchAny))
		COMPLETE_WITH("RENAME TO", "TO", "USING (", "WITH CHECK (");
	/* ALTER POLICY <name> ON <table> TO <role> */
	else if (Matches("ALTER", "POLICY", MatchAny, "ON", MatchAny, "TO"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
								 Keywords_for_list_of_grant_roles);
	/* ALTER POLICY <name> ON <table> USING ( */
	else if (Matches("ALTER", "POLICY", MatchAny, "ON", MatchAny, "USING"))
		COMPLETE_WITH("(");
	/* ALTER POLICY <name> ON <table> WITH CHECK ( */
	else if (Matches("ALTER", "POLICY", MatchAny, "ON", MatchAny, "WITH", "CHECK"))
		COMPLETE_WITH("(");

	/* ALTER RULE <name>, add ON */
	else if (Matches("ALTER", "RULE", MatchAny))
		COMPLETE_WITH("ON");

	/* If we have ALTER RULE <name> ON, then add the correct tablename */
	else if (Matches("ALTER", "RULE", MatchAny, "ON"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables_for_rule);
	}

	/* ALTER RULE <name> ON <name> */
	else if (Matches("ALTER", "RULE", MatchAny, "ON", MatchAny))
		COMPLETE_WITH("RENAME TO");

	/* ALTER STATISTICS <name> */
	else if (Matches("ALTER", "STATISTICS", MatchAny))
		COMPLETE_WITH("OWNER TO", "RENAME TO", "SET SCHEMA", "SET STATISTICS");
	/* ALTER STATISTICS <name> SET */
	else if (Matches("ALTER", "STATISTICS", MatchAny, "SET"))
		COMPLETE_WITH("SCHEMA", "STATISTICS");

	/* ALTER TRIGGER <name>, add ON */
	else if (Matches("ALTER", "TRIGGER", MatchAny))
		COMPLETE_WITH("ON");

	else if (Matches("ALTER", "TRIGGER", MatchAny, "ON"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables_for_trigger);
	}

	/* ALTER TRIGGER <name> ON <name> */
	else if (Matches("ALTER", "TRIGGER", MatchAny, "ON", MatchAny))
		COMPLETE_WITH("RENAME TO", "DEPENDS ON EXTENSION",
					  "NO DEPENDS ON EXTENSION");

	/*
	 * If we detect ALTER TABLE <name>, suggest sub commands
	 */
	else if (Matches("ALTER", "TABLE", MatchAny))
		COMPLETE_WITH("ADD", "ALTER", "CLUSTER ON", "DISABLE", "DROP",
					  "ENABLE", "INHERIT", "NO", "RENAME", "RESET",
					  "OWNER TO", "SET", "VALIDATE CONSTRAINT",
					  "REPLICA IDENTITY", "ATTACH PARTITION",
					  "DETACH PARTITION", "FORCE ROW LEVEL SECURITY",
					  "OF", "NOT OF");
	/* ALTER TABLE xxx ADD */
	else if (Matches("ALTER", "TABLE", MatchAny, "ADD"))
	{
		/* make sure to keep this list and the !Matches() below in sync */
		COMPLETE_WITH("COLUMN", "CONSTRAINT", "CHECK", "UNIQUE", "PRIMARY KEY",
					  "EXCLUDE", "FOREIGN KEY");
	}
	/* ALTER TABLE xxx ADD [COLUMN] yyy */
	else if (Matches("ALTER", "TABLE", MatchAny, "ADD", "COLUMN", MatchAny) ||
			 (Matches("ALTER", "TABLE", MatchAny, "ADD", MatchAny) &&
			  !Matches("ALTER", "TABLE", MatchAny, "ADD", "COLUMN|CONSTRAINT|CHECK|UNIQUE|PRIMARY|EXCLUDE|FOREIGN")))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
	/* ALTER TABLE xxx ADD CONSTRAINT yyy */
	else if (Matches("ALTER", "TABLE", MatchAny, "ADD", "CONSTRAINT", MatchAny))
		COMPLETE_WITH("CHECK", "UNIQUE", "PRIMARY KEY", "EXCLUDE", "FOREIGN KEY");
	/* ALTER TABLE xxx ADD [CONSTRAINT yyy] (PRIMARY KEY|UNIQUE) */
	else if (Matches("ALTER", "TABLE", MatchAny, "ADD", "PRIMARY", "KEY") ||
			 Matches("ALTER", "TABLE", MatchAny, "ADD", "UNIQUE") ||
			 Matches("ALTER", "TABLE", MatchAny, "ADD", "CONSTRAINT", MatchAny, "PRIMARY", "KEY") ||
			 Matches("ALTER", "TABLE", MatchAny, "ADD", "CONSTRAINT", MatchAny, "UNIQUE"))
		COMPLETE_WITH("(", "USING INDEX");
	/* ALTER TABLE xxx ADD PRIMARY KEY USING INDEX */
	else if (Matches("ALTER", "TABLE", MatchAny, "ADD", "PRIMARY", "KEY", "USING", "INDEX"))
	{
		set_completion_reference(prev6_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_unique_index_of_table);
	}
	/* ALTER TABLE xxx ADD UNIQUE USING INDEX */
	else if (Matches("ALTER", "TABLE", MatchAny, "ADD", "UNIQUE", "USING", "INDEX"))
	{
		set_completion_reference(prev5_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_unique_index_of_table);
	}
	/* ALTER TABLE xxx ADD CONSTRAINT yyy PRIMARY KEY USING INDEX */
	else if (Matches("ALTER", "TABLE", MatchAny, "ADD", "CONSTRAINT", MatchAny,
					 "PRIMARY", "KEY", "USING", "INDEX"))
	{
		set_completion_reference(prev8_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_unique_index_of_table);
	}
	/* ALTER TABLE xxx ADD CONSTRAINT yyy UNIQUE USING INDEX */
	else if (Matches("ALTER", "TABLE", MatchAny, "ADD", "CONSTRAINT", MatchAny,
					 "UNIQUE", "USING", "INDEX"))
	{
		set_completion_reference(prev7_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_unique_index_of_table);
	}
	/* ALTER TABLE xxx ENABLE */
	else if (Matches("ALTER", "TABLE", MatchAny, "ENABLE"))
		COMPLETE_WITH("ALWAYS", "REPLICA", "ROW LEVEL SECURITY", "RULE",
					  "TRIGGER");
	else if (Matches("ALTER", "TABLE", MatchAny, "ENABLE", "REPLICA|ALWAYS"))
		COMPLETE_WITH("RULE", "TRIGGER");
	else if (Matches("ALTER", "TABLE", MatchAny, "ENABLE", "RULE"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_rule_of_table);
	}
	else if (Matches("ALTER", "TABLE", MatchAny, "ENABLE", MatchAny, "RULE"))
	{
		set_completion_reference(prev4_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_rule_of_table);
	}
	else if (Matches("ALTER", "TABLE", MatchAny, "ENABLE", "TRIGGER"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_trigger_of_table);
	}
	else if (Matches("ALTER", "TABLE", MatchAny, "ENABLE", MatchAny, "TRIGGER"))
	{
		set_completion_reference(prev4_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_trigger_of_table);
	}
	/* ALTER TABLE xxx INHERIT */
	else if (Matches("ALTER", "TABLE", MatchAny, "INHERIT"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	/* ALTER TABLE xxx NO */
	else if (Matches("ALTER", "TABLE", MatchAny, "NO"))
		COMPLETE_WITH("FORCE ROW LEVEL SECURITY", "INHERIT");
	/* ALTER TABLE xxx NO INHERIT */
	else if (Matches("ALTER", "TABLE", MatchAny, "NO", "INHERIT"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	/* ALTER TABLE xxx DISABLE */
	else if (Matches("ALTER", "TABLE", MatchAny, "DISABLE"))
		COMPLETE_WITH("ROW LEVEL SECURITY", "RULE", "TRIGGER");
	else if (Matches("ALTER", "TABLE", MatchAny, "DISABLE", "RULE"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_rule_of_table);
	}
	else if (Matches("ALTER", "TABLE", MatchAny, "DISABLE", "TRIGGER"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_trigger_of_table);
	}

	/* ALTER TABLE xxx ALTER */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER"))
		COMPLETE_WITH_ATTR_PLUS(prev2_wd, "COLUMN", "CONSTRAINT");

	/* ALTER TABLE xxx RENAME */
	else if (Matches("ALTER", "TABLE", MatchAny, "RENAME"))
		COMPLETE_WITH_ATTR_PLUS(prev2_wd, "COLUMN", "CONSTRAINT", "TO");
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER|RENAME", "COLUMN"))
		COMPLETE_WITH_ATTR(prev3_wd);

	/* ALTER TABLE xxx RENAME yyy */
	else if (Matches("ALTER", "TABLE", MatchAny, "RENAME", MatchAnyExcept("CONSTRAINT|TO")))
		COMPLETE_WITH("TO");

	/* ALTER TABLE xxx RENAME COLUMN/CONSTRAINT yyy */
	else if (Matches("ALTER", "TABLE", MatchAny, "RENAME", "COLUMN|CONSTRAINT", MatchAnyExcept("TO")))
		COMPLETE_WITH("TO");

	/* If we have ALTER TABLE <sth> DROP, provide COLUMN or CONSTRAINT */
	else if (Matches("ALTER", "TABLE", MatchAny, "DROP"))
		COMPLETE_WITH("COLUMN", "CONSTRAINT");
	/* If we have ALTER TABLE <sth> DROP COLUMN, provide list of columns */
	else if (Matches("ALTER", "TABLE", MatchAny, "DROP", "COLUMN"))
		COMPLETE_WITH_ATTR(prev3_wd);
	/* ALTER TABLE <sth> ALTER|DROP|RENAME CONSTRAINT <constraint> */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER|DROP|RENAME", "CONSTRAINT"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_constraint_of_table);
	}
	/* ALTER TABLE <sth> VALIDATE CONSTRAINT <non-validated constraint> */
	else if (Matches("ALTER", "TABLE", MatchAny, "VALIDATE", "CONSTRAINT"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_constraint_of_table_not_validated);
	}
	/* ALTER TABLE ALTER [COLUMN] <foo> */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny) ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny))
		COMPLETE_WITH("TYPE", "SET", "RESET", "RESTART", "ADD", "DROP");
	/* ALTER TABLE ALTER [COLUMN] <foo> ADD */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "ADD") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "ADD"))
		COMPLETE_WITH("GENERATED");
	/* ALTER TABLE ALTER [COLUMN] <foo> ADD GENERATED */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "ADD", "GENERATED") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "ADD", "GENERATED"))
		COMPLETE_WITH("ALWAYS", "BY DEFAULT");
	/* ALTER TABLE ALTER [COLUMN] <foo> ADD GENERATED */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "ADD", "GENERATED", "ALWAYS") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "ADD", "GENERATED", "ALWAYS") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "ADD", "GENERATED", "BY", "DEFAULT") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "ADD", "GENERATED", "BY", "DEFAULT"))
		COMPLETE_WITH("AS IDENTITY");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET"))
		COMPLETE_WITH("(", "COMPRESSION", "DATA TYPE", "DEFAULT", "EXPRESSION", "GENERATED", "NOT NULL",
					  "STATISTICS", "STORAGE",
		/* a subset of ALTER SEQUENCE options */
					  "INCREMENT", "MINVALUE", "MAXVALUE", "START", "NO", "CACHE", "CYCLE");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET ( */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "(") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET", "("))
		COMPLETE_WITH("n_distinct", "n_distinct_inherited");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET COMPRESSION */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "COMPRESSION") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET", "COMPRESSION"))
		COMPLETE_WITH("DEFAULT", "PGLZ", "LZ4");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET EXPRESSION */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "EXPRESSION") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET", "EXPRESSION"))
		COMPLETE_WITH("AS");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET EXPRESSION AS */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "EXPRESSION", "AS") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET", "EXPRESSION", "AS"))
		COMPLETE_WITH("(");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET GENERATED */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "GENERATED") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET", "GENERATED"))
		COMPLETE_WITH("ALWAYS", "BY DEFAULT");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET NO */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "NO") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET", "NO"))
		COMPLETE_WITH("MINVALUE", "MAXVALUE", "CYCLE");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET STORAGE */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "STORAGE") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET", "STORAGE"))
		COMPLETE_WITH("DEFAULT", "PLAIN", "EXTERNAL", "EXTENDED", "MAIN");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET STATISTICS */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "STATISTICS") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET", "STATISTICS"))
	{
		/* Enforce no completion here, as an integer has to be specified */
	}
	/* ALTER TABLE ALTER [COLUMN] <foo> DROP */
	else if (Matches("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "DROP") ||
			 Matches("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "DROP"))
		COMPLETE_WITH("DEFAULT", "EXPRESSION", "IDENTITY", "NOT NULL");
	else if (Matches("ALTER", "TABLE", MatchAny, "CLUSTER"))
		COMPLETE_WITH("ON");
	else if (Matches("ALTER", "TABLE", MatchAny, "CLUSTER", "ON"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_index_of_table);
	}
	/* If we have ALTER TABLE <sth> SET, provide list of attributes and '(' */
	else if (Matches("ALTER", "TABLE", MatchAny, "SET"))
		COMPLETE_WITH("(", "ACCESS METHOD", "LOGGED", "SCHEMA",
					  "TABLESPACE", "UNLOGGED", "WITH", "WITHOUT");

	/*
	 * If we have ALTER TABLE <sth> SET ACCESS METHOD provide a list of table
	 * AMs.
	 */
	else if (Matches("ALTER", "TABLE", MatchAny, "SET", "ACCESS", "METHOD"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_table_access_methods,
								 "DEFAULT");

	/*
	 * If we have ALTER TABLE <sth> SET TABLESPACE provide a list of
	 * tablespaces
	 */
	else if (Matches("ALTER", "TABLE", MatchAny, "SET", "TABLESPACE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
	/* If we have ALTER TABLE <sth> SET WITHOUT provide CLUSTER or OIDS */
	else if (Matches("ALTER", "TABLE", MatchAny, "SET", "WITHOUT"))
		COMPLETE_WITH("CLUSTER", "OIDS");
	/* ALTER TABLE <foo> RESET */
	else if (Matches("ALTER", "TABLE", MatchAny, "RESET"))
		COMPLETE_WITH("(");
	/* ALTER TABLE <foo> SET|RESET ( */
	else if (Matches("ALTER", "TABLE", MatchAny, "SET|RESET", "("))
		COMPLETE_WITH_LIST(table_storage_parameters);
	else if (Matches("ALTER", "TABLE", MatchAny, "REPLICA", "IDENTITY", "USING", "INDEX"))
	{
		set_completion_reference(prev5_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_index_of_table);
	}
	else if (Matches("ALTER", "TABLE", MatchAny, "REPLICA", "IDENTITY", "USING"))
		COMPLETE_WITH("INDEX");
	else if (Matches("ALTER", "TABLE", MatchAny, "REPLICA", "IDENTITY"))
		COMPLETE_WITH("FULL", "NOTHING", "DEFAULT", "USING");
	else if (Matches("ALTER", "TABLE", MatchAny, "REPLICA"))
		COMPLETE_WITH("IDENTITY");

	/*
	 * If we have ALTER TABLE <foo> ATTACH PARTITION, provide a list of
	 * tables.
	 */
	else if (Matches("ALTER", "TABLE", MatchAny, "ATTACH", "PARTITION"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	/* Limited completion support for partition bound specification */
	else if (TailMatches("ATTACH", "PARTITION", MatchAny))
		COMPLETE_WITH("FOR VALUES", "DEFAULT");
	else if (TailMatches("FOR", "VALUES"))
		COMPLETE_WITH("FROM (", "IN (", "WITH (");

	/*
	 * If we have ALTER TABLE <foo> DETACH PARTITION, provide a list of
	 * partitions of <foo>.
	 */
	else if (Matches("ALTER", "TABLE", MatchAny, "DETACH", "PARTITION"))
	{
		set_completion_reference(prev3_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_partition_of_table);
	}
	else if (Matches("ALTER", "TABLE", MatchAny, "DETACH", "PARTITION", MatchAny))
		COMPLETE_WITH("CONCURRENTLY", "FINALIZE");

	/* ALTER TABLE <name> OF */
	else if (Matches("ALTER", "TABLE", MatchAny, "OF"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_composite_datatypes);

	/* ALTER TABLESPACE <foo> with RENAME TO, OWNER TO, SET, RESET */
	else if (Matches("ALTER", "TABLESPACE", MatchAny))
		COMPLETE_WITH("RENAME TO", "OWNER TO", "SET", "RESET");
	/* ALTER TABLESPACE <foo> SET|RESET */
	else if (Matches("ALTER", "TABLESPACE", MatchAny, "SET|RESET"))
		COMPLETE_WITH("(");
	/* ALTER TABLESPACE <foo> SET|RESET ( */
	else if (Matches("ALTER", "TABLESPACE", MatchAny, "SET|RESET", "("))
		COMPLETE_WITH("seq_page_cost", "random_page_cost",
					  "effective_io_concurrency", "maintenance_io_concurrency");

	/* ALTER TEXT SEARCH */
	else if (Matches("ALTER", "TEXT", "SEARCH"))
		COMPLETE_WITH("CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE");
	else if (Matches("ALTER", "TEXT", "SEARCH", "TEMPLATE|PARSER", MatchAny))
		COMPLETE_WITH("RENAME TO", "SET SCHEMA");
	else if (Matches("ALTER", "TEXT", "SEARCH", "DICTIONARY", MatchAny))
		COMPLETE_WITH("(", "OWNER TO", "RENAME TO", "SET SCHEMA");
	else if (Matches("ALTER", "TEXT", "SEARCH", "CONFIGURATION", MatchAny))
		COMPLETE_WITH("ADD MAPPING FOR", "ALTER MAPPING",
					  "DROP MAPPING FOR",
					  "OWNER TO", "RENAME TO", "SET SCHEMA");

	/* complete ALTER TYPE <foo> with actions */
	else if (Matches("ALTER", "TYPE", MatchAny))
		COMPLETE_WITH("ADD ATTRIBUTE", "ADD VALUE", "ALTER ATTRIBUTE",
					  "DROP ATTRIBUTE",
					  "OWNER TO", "RENAME", "SET SCHEMA", "SET (");
	/* complete ALTER TYPE <foo> ADD with actions */
	else if (Matches("ALTER", "TYPE", MatchAny, "ADD"))
		COMPLETE_WITH("ATTRIBUTE", "VALUE");
	/* ALTER TYPE <foo> RENAME	*/
	else if (Matches("ALTER", "TYPE", MatchAny, "RENAME"))
		COMPLETE_WITH("ATTRIBUTE", "TO", "VALUE");
	/* ALTER TYPE xxx RENAME (ATTRIBUTE|VALUE) yyy */
	else if (Matches("ALTER", "TYPE", MatchAny, "RENAME", "ATTRIBUTE|VALUE", MatchAny))
		COMPLETE_WITH("TO");

	/*
	 * If we have ALTER TYPE <sth> ALTER/DROP/RENAME ATTRIBUTE, provide list
	 * of attributes
	 */
	else if (Matches("ALTER", "TYPE", MatchAny, "ALTER|DROP|RENAME", "ATTRIBUTE"))
		COMPLETE_WITH_ATTR(prev3_wd);
	/* ALTER TYPE ALTER ATTRIBUTE <foo> */
	else if (Matches("ALTER", "TYPE", MatchAny, "ALTER", "ATTRIBUTE", MatchAny))
		COMPLETE_WITH("TYPE");
	/* complete ALTER TYPE <sth> RENAME VALUE with list of enum values */
	else if (Matches("ALTER", "TYPE", MatchAny, "RENAME", "VALUE"))
		COMPLETE_WITH_ENUM_VALUE(prev3_wd);
	/* ALTER TYPE <foo> SET */
	else if (Matches("ALTER", "TYPE", MatchAny, "SET"))
		COMPLETE_WITH("(", "SCHEMA");
	/* complete ALTER TYPE <foo> SET ( with settable properties */
	else if (Matches("ALTER", "TYPE", MatchAny, "SET", "("))
		COMPLETE_WITH("ANALYZE", "RECEIVE", "SEND", "STORAGE", "SUBSCRIPT",
					  "TYPMOD_IN", "TYPMOD_OUT");

	/* complete ALTER GROUP <foo> */
	else if (Matches("ALTER", "GROUP", MatchAny))
		COMPLETE_WITH("ADD USER", "DROP USER", "RENAME TO");
	/* complete ALTER GROUP <foo> ADD|DROP with USER */
	else if (Matches("ALTER", "GROUP", MatchAny, "ADD|DROP"))
		COMPLETE_WITH("USER");
	/* complete ALTER GROUP <foo> ADD|DROP USER with a user name */
	else if (Matches("ALTER", "GROUP", MatchAny, "ADD|DROP", "USER"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);

/*
 * ANALYZE [ ( option [, ...] ) ] [ table_and_columns [, ...] ]
 * ANALYZE [ VERBOSE ] [ table_and_columns [, ...] ]
 */
	else if (Matches("ANALYZE"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_analyzables,
										"VERBOSE");
	else if (HeadMatches("ANALYZE", "(*") &&
			 !HeadMatches("ANALYZE", "(*)"))
	{
		/*
		 * This fires if we're in an unfinished parenthesized option list.
		 * get_previous_words treats a completed parenthesized option list as
		 * one word, so the above test is correct.
		 */
		if (ends_with(prev_wd, '(') || ends_with(prev_wd, ','))
			COMPLETE_WITH("VERBOSE", "SKIP_LOCKED", "BUFFER_USAGE_LIMIT");
		else if (TailMatches("VERBOSE|SKIP_LOCKED"))
			COMPLETE_WITH("ON", "OFF");
	}
	else if (HeadMatches("ANALYZE") && TailMatches("("))
		/* "ANALYZE (" should be caught above, so assume we want columns */
		COMPLETE_WITH_ATTR(prev2_wd);
	else if (HeadMatches("ANALYZE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_analyzables);

/* BEGIN */
	else if (Matches("BEGIN"))
		COMPLETE_WITH("WORK", "TRANSACTION", "ISOLATION LEVEL", "READ", "DEFERRABLE", "NOT DEFERRABLE");
/* END, ABORT */
	else if (Matches("END|ABORT"))
		COMPLETE_WITH("AND", "WORK", "TRANSACTION");
/* COMMIT */
	else if (Matches("COMMIT"))
		COMPLETE_WITH("AND", "WORK", "TRANSACTION", "PREPARED");
/* RELEASE SAVEPOINT */
	else if (Matches("RELEASE"))
		COMPLETE_WITH("SAVEPOINT");
/* ROLLBACK */
	else if (Matches("ROLLBACK"))
		COMPLETE_WITH("AND", "WORK", "TRANSACTION", "TO SAVEPOINT", "PREPARED");
	else if (Matches("ABORT|END|COMMIT|ROLLBACK", "AND"))
		COMPLETE_WITH("CHAIN");
/* CALL */
	else if (Matches("CALL"))
		COMPLETE_WITH_VERSIONED_SCHEMA_QUERY(Query_for_list_of_procedures);
	else if (Matches("CALL", MatchAny))
		COMPLETE_WITH("(");
/* CLOSE */
	else if (Matches("CLOSE"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_cursors,
								 "ALL");
/* CLUSTER */
	else if (Matches("CLUSTER"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_clusterables,
										"VERBOSE");
	else if (Matches("CLUSTER", "VERBOSE") ||
			 Matches("CLUSTER", "(*)"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_clusterables);
	/* If we have CLUSTER <sth>, then add "USING" */
	else if (Matches("CLUSTER", MatchAnyExcept("VERBOSE|ON|(|(*)")))
		COMPLETE_WITH("USING");
	/* If we have CLUSTER VERBOSE <sth>, then add "USING" */
	else if (Matches("CLUSTER", "VERBOSE|(*)", MatchAny))
		COMPLETE_WITH("USING");
	/* If we have CLUSTER <sth> USING, then add the index as well */
	else if (Matches("CLUSTER", MatchAny, "USING") ||
			 Matches("CLUSTER", "VERBOSE|(*)", MatchAny, "USING"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_index_of_table);
	}
	else if (HeadMatches("CLUSTER", "(*") &&
			 !HeadMatches("CLUSTER", "(*)"))
	{
		/*
		 * This fires if we're in an unfinished parenthesized option list.
		 * get_previous_words treats a completed parenthesized option list as
		 * one word, so the above test is correct.
		 */
		if (ends_with(prev_wd, '(') || ends_with(prev_wd, ','))
			COMPLETE_WITH("VERBOSE");
	}

/* COMMENT */
	else if (Matches("COMMENT"))
		COMPLETE_WITH("ON");
	else if (Matches("COMMENT", "ON"))
		COMPLETE_WITH("ACCESS METHOD", "AGGREGATE", "CAST", "COLLATION",
					  "COLUMN", "CONSTRAINT", "CONVERSION", "DATABASE",
					  "DOMAIN", "EXTENSION", "EVENT TRIGGER",
					  "FOREIGN DATA WRAPPER", "FOREIGN TABLE",
					  "FUNCTION", "INDEX", "LANGUAGE", "LARGE OBJECT",
					  "MATERIALIZED VIEW", "OPERATOR", "POLICY",
					  "PROCEDURE", "PROCEDURAL LANGUAGE", "PUBLICATION", "ROLE",
					  "ROUTINE", "RULE", "SCHEMA", "SEQUENCE", "SERVER",
					  "STATISTICS", "SUBSCRIPTION", "TABLE",
					  "TABLESPACE", "TEXT SEARCH", "TRANSFORM FOR",
					  "TRIGGER", "TYPE", "VIEW");
	else if (Matches("COMMENT", "ON", "ACCESS", "METHOD"))
		COMPLETE_WITH_QUERY(Query_for_list_of_access_methods);
	else if (Matches("COMMENT", "ON", "CONSTRAINT"))
		COMPLETE_WITH_QUERY(Query_for_all_table_constraints);
	else if (Matches("COMMENT", "ON", "CONSTRAINT", MatchAny))
		COMPLETE_WITH("ON");
	else if (Matches("COMMENT", "ON", "CONSTRAINT", MatchAny, "ON"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_tables_for_constraint,
										"DOMAIN");
	}
	else if (Matches("COMMENT", "ON", "CONSTRAINT", MatchAny, "ON", "DOMAIN"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_domains);
	else if (Matches("COMMENT", "ON", "EVENT", "TRIGGER"))
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);
	else if (Matches("COMMENT", "ON", "FOREIGN"))
		COMPLETE_WITH("DATA WRAPPER", "TABLE");
	else if (Matches("COMMENT", "ON", "FOREIGN", "TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_foreign_tables);
	else if (Matches("COMMENT", "ON", "MATERIALIZED", "VIEW"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews);
	else if (Matches("COMMENT", "ON", "POLICY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_policies);
	else if (Matches("COMMENT", "ON", "POLICY", MatchAny))
		COMPLETE_WITH("ON");
	else if (Matches("COMMENT", "ON", "POLICY", MatchAny, "ON"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables_for_policy);
	}
	else if (Matches("COMMENT", "ON", "PROCEDURAL", "LANGUAGE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_languages);
	else if (Matches("COMMENT", "ON", "RULE", MatchAny))
		COMPLETE_WITH("ON");
	else if (Matches("COMMENT", "ON", "RULE", MatchAny, "ON"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables_for_rule);
	}
	else if (Matches("COMMENT", "ON", "TEXT", "SEARCH"))
		COMPLETE_WITH("CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE");
	else if (Matches("COMMENT", "ON", "TEXT", "SEARCH", "CONFIGURATION"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_ts_configurations);
	else if (Matches("COMMENT", "ON", "TEXT", "SEARCH", "DICTIONARY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_ts_dictionaries);
	else if (Matches("COMMENT", "ON", "TEXT", "SEARCH", "PARSER"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_ts_parsers);
	else if (Matches("COMMENT", "ON", "TEXT", "SEARCH", "TEMPLATE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_ts_templates);
	else if (Matches("COMMENT", "ON", "TRANSFORM", "FOR"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
	else if (Matches("COMMENT", "ON", "TRANSFORM", "FOR", MatchAny))
		COMPLETE_WITH("LANGUAGE");
	else if (Matches("COMMENT", "ON", "TRANSFORM", "FOR", MatchAny, "LANGUAGE"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_QUERY(Query_for_list_of_languages);
	}
	else if (Matches("COMMENT", "ON", "TRIGGER", MatchAny))
		COMPLETE_WITH("ON");
	else if (Matches("COMMENT", "ON", "TRIGGER", MatchAny, "ON"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables_for_trigger);
	}
	else if (Matches("COMMENT", "ON", MatchAny, MatchAnyExcept("IS")) ||
			 Matches("COMMENT", "ON", MatchAny, MatchAny, MatchAnyExcept("IS")) ||
			 Matches("COMMENT", "ON", MatchAny, MatchAny, MatchAny, MatchAnyExcept("IS")) ||
			 Matches("COMMENT", "ON", MatchAny, MatchAny, MatchAny, MatchAny, MatchAnyExcept("IS")))
		COMPLETE_WITH("IS");

/* COPY */

	/*
	 * If we have COPY, offer list of tables or "(" (Also cover the analogous
	 * backslash command).
	 */
	else if (Matches("COPY|\\copy"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_tables, "(");
	/* Complete COPY ( with legal query commands */
	else if (Matches("COPY|\\copy", "("))
		COMPLETE_WITH("SELECT", "TABLE", "VALUES", "INSERT INTO", "UPDATE", "DELETE FROM", "WITH");
	/* Complete COPY <sth> */
	else if (Matches("COPY|\\copy", MatchAny))
		COMPLETE_WITH("FROM", "TO");
	/* Complete COPY <sth> FROM|TO with filename */
	else if (Matches("COPY", MatchAny, "FROM|TO"))
	{
		completion_charp = "";
		completion_force_quote = true;	/* COPY requires quoted filename */
		matches = rl_completion_matches(text, complete_from_files);
	}
	else if (Matches("\\copy", MatchAny, "FROM|TO"))
	{
		completion_charp = "";
		completion_force_quote = false;
		matches = rl_completion_matches(text, complete_from_files);
	}

	/* Complete COPY <sth> TO <sth> */
	else if (Matches("COPY|\\copy", MatchAny, "TO", MatchAny))
		COMPLETE_WITH("WITH (");

	/* Complete COPY <sth> FROM <sth> */
	else if (Matches("COPY|\\copy", MatchAny, "FROM", MatchAny))
		COMPLETE_WITH("WITH (", "WHERE");

	/* Complete COPY <sth> FROM|TO filename WITH ( */
	else if (Matches("COPY|\\copy", MatchAny, "FROM|TO", MatchAny, "WITH", "("))
		COMPLETE_WITH("FORMAT", "FREEZE", "DELIMITER", "NULL",
					  "HEADER", "QUOTE", "ESCAPE", "FORCE_QUOTE",
					  "FORCE_NOT_NULL", "FORCE_NULL", "ENCODING", "DEFAULT",
					  "ON_ERROR", "LOG_VERBOSITY");

	/* Complete COPY <sth> FROM|TO filename WITH (FORMAT */
	else if (Matches("COPY|\\copy", MatchAny, "FROM|TO", MatchAny, "WITH", "(", "FORMAT"))
		COMPLETE_WITH("binary", "csv", "text");

	/* Complete COPY <sth> FROM filename WITH (ON_ERROR */
	else if (Matches("COPY|\\copy", MatchAny, "FROM|TO", MatchAny, "WITH", "(", "ON_ERROR"))
		COMPLETE_WITH("stop", "ignore");

	/* Complete COPY <sth> FROM filename WITH (LOG_VERBOSITY */
	else if (Matches("COPY|\\copy", MatchAny, "FROM|TO", MatchAny, "WITH", "(", "LOG_VERBOSITY"))
		COMPLETE_WITH("default", "verbose");

	/* Complete COPY <sth> FROM <sth> WITH (<options>) */
	else if (Matches("COPY|\\copy", MatchAny, "FROM", MatchAny, "WITH", MatchAny))
		COMPLETE_WITH("WHERE");

	/* CREATE ACCESS METHOD */
	/* Complete "CREATE ACCESS METHOD <name>" */
	else if (Matches("CREATE", "ACCESS", "METHOD", MatchAny))
		COMPLETE_WITH("TYPE");
	/* Complete "CREATE ACCESS METHOD <name> TYPE" */
	else if (Matches("CREATE", "ACCESS", "METHOD", MatchAny, "TYPE"))
		COMPLETE_WITH("INDEX", "TABLE");
	/* Complete "CREATE ACCESS METHOD <name> TYPE <type>" */
	else if (Matches("CREATE", "ACCESS", "METHOD", MatchAny, "TYPE", MatchAny))
		COMPLETE_WITH("HANDLER");

	/* CREATE COLLATION */
	else if (Matches("CREATE", "COLLATION", MatchAny))
		COMPLETE_WITH("(", "FROM");
	else if (Matches("CREATE", "COLLATION", MatchAny, "FROM"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_collations);
	else if (HeadMatches("CREATE", "COLLATION", MatchAny, "(*"))
	{
		if (TailMatches("(|*,"))
			COMPLETE_WITH("LOCALE =", "LC_COLLATE =", "LC_CTYPE =",
						  "PROVIDER =", "DETERMINISTIC =");
		else if (TailMatches("PROVIDER", "="))
			COMPLETE_WITH("libc", "icu");
		else if (TailMatches("DETERMINISTIC", "="))
			COMPLETE_WITH("true", "false");
	}

	/* CREATE DATABASE */
	else if (Matches("CREATE", "DATABASE", MatchAny))
		COMPLETE_WITH("OWNER", "TEMPLATE", "ENCODING", "TABLESPACE",
					  "IS_TEMPLATE", "STRATEGY",
					  "ALLOW_CONNECTIONS", "CONNECTION LIMIT",
					  "LC_COLLATE", "LC_CTYPE", "LOCALE", "OID",
					  "LOCALE_PROVIDER", "ICU_LOCALE");

	else if (Matches("CREATE", "DATABASE", MatchAny, "TEMPLATE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_template_databases);
	else if (Matches("CREATE", "DATABASE", MatchAny, "STRATEGY"))
		COMPLETE_WITH("WAL_LOG", "FILE_COPY");

	/* CREATE DOMAIN */
	else if (Matches("CREATE", "DOMAIN", MatchAny))
		COMPLETE_WITH("AS");
	else if (Matches("CREATE", "DOMAIN", MatchAny, "AS"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
	else if (Matches("CREATE", "DOMAIN", MatchAny, "AS", MatchAny))
		COMPLETE_WITH("COLLATE", "DEFAULT", "CONSTRAINT",
					  "NOT NULL", "NULL", "CHECK (");
	else if (Matches("CREATE", "DOMAIN", MatchAny, "COLLATE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_collations);

	/* CREATE EXTENSION */
	/* Complete with available extensions rather than installed ones. */
	else if (Matches("CREATE", "EXTENSION"))
		COMPLETE_WITH_QUERY(Query_for_list_of_available_extensions);
	/* CREATE EXTENSION <name> */
	else if (Matches("CREATE", "EXTENSION", MatchAny))
		COMPLETE_WITH("WITH SCHEMA", "CASCADE", "VERSION");
	/* CREATE EXTENSION <name> VERSION */
	else if (Matches("CREATE", "EXTENSION", MatchAny, "VERSION"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_QUERY(Query_for_list_of_available_extension_versions);
	}

	/* CREATE FOREIGN */
	else if (Matches("CREATE", "FOREIGN"))
		COMPLETE_WITH("DATA WRAPPER", "TABLE");

	/* CREATE FOREIGN DATA WRAPPER */
	else if (Matches("CREATE", "FOREIGN", "DATA", "WRAPPER", MatchAny))
		COMPLETE_WITH("HANDLER", "VALIDATOR", "OPTIONS");

	/* CREATE FOREIGN TABLE */
	else if (Matches("CREATE", "FOREIGN", "TABLE", MatchAny))
		COMPLETE_WITH("(", "PARTITION OF");

	/* CREATE INDEX --- is allowed inside CREATE SCHEMA, so use TailMatches */
	/* First off we complete CREATE UNIQUE with "INDEX" */
	else if (TailMatches("CREATE", "UNIQUE"))
		COMPLETE_WITH("INDEX");

	/*
	 * If we have CREATE|UNIQUE INDEX, then add "ON", "CONCURRENTLY", and
	 * existing indexes
	 */
	else if (TailMatches("CREATE|UNIQUE", "INDEX"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_indexes,
										"ON", "CONCURRENTLY");

	/*
	 * Complete ... INDEX|CONCURRENTLY [<name>] ON with a list of relations
	 * that indexes can be created on
	 */
	else if (TailMatches("INDEX|CONCURRENTLY", MatchAny, "ON") ||
			 TailMatches("INDEX|CONCURRENTLY", "ON"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexables);

	/*
	 * Complete CREATE|UNIQUE INDEX CONCURRENTLY with "ON" and existing
	 * indexes
	 */
	else if (TailMatches("CREATE|UNIQUE", "INDEX", "CONCURRENTLY"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_indexes,
										"ON");
	/* Complete CREATE|UNIQUE INDEX [CONCURRENTLY] <sth> with "ON" */
	else if (TailMatches("CREATE|UNIQUE", "INDEX", MatchAny) ||
			 TailMatches("CREATE|UNIQUE", "INDEX", "CONCURRENTLY", MatchAny))
		COMPLETE_WITH("ON");

	/*
	 * Complete INDEX <name> ON <table> with a list of table columns (which
	 * should really be in parens)
	 */
	else if (TailMatches("INDEX", MatchAny, "ON", MatchAny) ||
			 TailMatches("INDEX|CONCURRENTLY", "ON", MatchAny))
		COMPLETE_WITH("(", "USING");
	else if (TailMatches("INDEX", MatchAny, "ON", MatchAny, "(") ||
			 TailMatches("INDEX|CONCURRENTLY", "ON", MatchAny, "("))
		COMPLETE_WITH_ATTR(prev2_wd);
	/* same if you put in USING */
	else if (TailMatches("ON", MatchAny, "USING", MatchAny, "("))
		COMPLETE_WITH_ATTR(prev4_wd);
	/* Complete USING with an index method */
	else if (TailMatches("INDEX", MatchAny, MatchAny, "ON", MatchAny, "USING") ||
			 TailMatches("INDEX", MatchAny, "ON", MatchAny, "USING") ||
			 TailMatches("INDEX", "ON", MatchAny, "USING"))
		COMPLETE_WITH_QUERY(Query_for_list_of_index_access_methods);
	else if (TailMatches("ON", MatchAny, "USING", MatchAny) &&
			 !TailMatches("POLICY", MatchAny, MatchAny, MatchAny, MatchAny, MatchAny) &&
			 !TailMatches("FOR", MatchAny, MatchAny, MatchAny))
		COMPLETE_WITH("(");

	/* CREATE OR REPLACE */
	else if (Matches("CREATE", "OR"))
		COMPLETE_WITH("REPLACE");

	/* CREATE POLICY */
	/* Complete "CREATE POLICY <name> ON" */
	else if (Matches("CREATE", "POLICY", MatchAny))
		COMPLETE_WITH("ON");
	/* Complete "CREATE POLICY <name> ON <table>" */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	/* Complete "CREATE POLICY <name> ON <table> AS|FOR|TO|USING|WITH CHECK" */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny))
		COMPLETE_WITH("AS", "FOR", "TO", "USING (", "WITH CHECK (");
	/* CREATE POLICY <name> ON <table> AS PERMISSIVE|RESTRICTIVE */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "AS"))
		COMPLETE_WITH("PERMISSIVE", "RESTRICTIVE");

	/*
	 * CREATE POLICY <name> ON <table> AS PERMISSIVE|RESTRICTIVE
	 * FOR|TO|USING|WITH CHECK
	 */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "AS", MatchAny))
		COMPLETE_WITH("FOR", "TO", "USING", "WITH CHECK");
	/* CREATE POLICY <name> ON <table> FOR ALL|SELECT|INSERT|UPDATE|DELETE */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "FOR"))
		COMPLETE_WITH("ALL", "SELECT", "INSERT", "UPDATE", "DELETE");
	/* Complete "CREATE POLICY <name> ON <table> FOR INSERT TO|WITH CHECK" */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "FOR", "INSERT"))
		COMPLETE_WITH("TO", "WITH CHECK (");
	/* Complete "CREATE POLICY <name> ON <table> FOR SELECT|DELETE TO|USING" */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "FOR", "SELECT|DELETE"))
		COMPLETE_WITH("TO", "USING (");
	/* CREATE POLICY <name> ON <table> FOR ALL|UPDATE TO|USING|WITH CHECK */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "FOR", "ALL|UPDATE"))
		COMPLETE_WITH("TO", "USING (", "WITH CHECK (");
	/* Complete "CREATE POLICY <name> ON <table> TO <role>" */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "TO"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
								 Keywords_for_list_of_grant_roles);
	/* Complete "CREATE POLICY <name> ON <table> USING (" */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "USING"))
		COMPLETE_WITH("(");

	/*
	 * CREATE POLICY <name> ON <table> AS PERMISSIVE|RESTRICTIVE FOR
	 * ALL|SELECT|INSERT|UPDATE|DELETE
	 */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "AS", MatchAny, "FOR"))
		COMPLETE_WITH("ALL", "SELECT", "INSERT", "UPDATE", "DELETE");

	/*
	 * Complete "CREATE POLICY <name> ON <table> AS PERMISSIVE|RESTRICTIVE FOR
	 * INSERT TO|WITH CHECK"
	 */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "AS", MatchAny, "FOR", "INSERT"))
		COMPLETE_WITH("TO", "WITH CHECK (");

	/*
	 * Complete "CREATE POLICY <name> ON <table> AS PERMISSIVE|RESTRICTIVE FOR
	 * SELECT|DELETE TO|USING"
	 */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "AS", MatchAny, "FOR", "SELECT|DELETE"))
		COMPLETE_WITH("TO", "USING (");

	/*
	 * CREATE POLICY <name> ON <table> AS PERMISSIVE|RESTRICTIVE FOR
	 * ALL|UPDATE TO|USING|WITH CHECK
	 */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "AS", MatchAny, "FOR", "ALL|UPDATE"))
		COMPLETE_WITH("TO", "USING (", "WITH CHECK (");

	/*
	 * Complete "CREATE POLICY <name> ON <table> AS PERMISSIVE|RESTRICTIVE TO
	 * <role>"
	 */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "AS", MatchAny, "TO"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
								 Keywords_for_list_of_grant_roles);

	/*
	 * Complete "CREATE POLICY <name> ON <table> AS PERMISSIVE|RESTRICTIVE
	 * USING ("
	 */
	else if (Matches("CREATE", "POLICY", MatchAny, "ON", MatchAny, "AS", MatchAny, "USING"))
		COMPLETE_WITH("(");


/* CREATE PUBLICATION */
	else if (Matches("CREATE", "PUBLICATION", MatchAny))
		COMPLETE_WITH("FOR TABLE", "FOR ALL TABLES", "FOR TABLES IN SCHEMA", "WITH (");
	else if (Matches("CREATE", "PUBLICATION", MatchAny, "FOR"))
		COMPLETE_WITH("TABLE", "ALL TABLES", "TABLES IN SCHEMA");
	else if (Matches("CREATE", "PUBLICATION", MatchAny, "FOR", "ALL"))
		COMPLETE_WITH("TABLES");
	else if (Matches("CREATE", "PUBLICATION", MatchAny, "FOR", "ALL", "TABLES"))
		COMPLETE_WITH("WITH (");
	else if (Matches("CREATE", "PUBLICATION", MatchAny, "FOR", "TABLES"))
		COMPLETE_WITH("IN SCHEMA");
	else if (Matches("CREATE", "PUBLICATION", MatchAny, "FOR", "TABLE", MatchAny) && !ends_with(prev_wd, ','))
		COMPLETE_WITH("WHERE (", "WITH (");
	/* Complete "CREATE PUBLICATION <name> FOR TABLE" with "<table>, ..." */
	else if (Matches("CREATE", "PUBLICATION", MatchAny, "FOR", "TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

	/*
	 * "CREATE PUBLICATION <name> FOR TABLE <name> WHERE (" - complete with
	 * table attributes
	 */
	else if (HeadMatches("CREATE", "PUBLICATION", MatchAny) && TailMatches("WHERE"))
		COMPLETE_WITH("(");
	else if (HeadMatches("CREATE", "PUBLICATION", MatchAny) && TailMatches("WHERE", "("))
		COMPLETE_WITH_ATTR(prev3_wd);
	else if (HeadMatches("CREATE", "PUBLICATION", MatchAny) && TailMatches("WHERE", "(*)"))
		COMPLETE_WITH(" WITH (");

	/*
	 * Complete "CREATE PUBLICATION <name> FOR TABLES IN SCHEMA <schema>, ..."
	 */
	else if (Matches("CREATE", "PUBLICATION", MatchAny, "FOR", "TABLES", "IN", "SCHEMA"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_schemas
								 " AND nspname NOT LIKE E'pg\\\\_%%'",
								 "CURRENT_SCHEMA");
	else if (Matches("CREATE", "PUBLICATION", MatchAny, "FOR", "TABLES", "IN", "SCHEMA", MatchAny) && (!ends_with(prev_wd, ',')))
		COMPLETE_WITH("WITH (");
	/* Complete "CREATE PUBLICATION <name> [...] WITH" */
	else if (HeadMatches("CREATE", "PUBLICATION") && TailMatches("WITH", "("))
		COMPLETE_WITH("publish", "publish_via_partition_root");

/* CREATE RULE */
	/* Complete "CREATE [ OR REPLACE ] RULE <sth>" with "AS ON" */
	else if (Matches("CREATE", "RULE", MatchAny) ||
			 Matches("CREATE", "OR", "REPLACE", "RULE", MatchAny))
		COMPLETE_WITH("AS ON");
	/* Complete "CREATE [ OR REPLACE ] RULE <sth> AS" with "ON" */
	else if (Matches("CREATE", "RULE", MatchAny, "AS") ||
			 Matches("CREATE", "OR", "REPLACE", "RULE", MatchAny, "AS"))
		COMPLETE_WITH("ON");

	/*
	 * Complete "CREATE [ OR REPLACE ] RULE <sth> AS ON" with
	 * SELECT|UPDATE|INSERT|DELETE
	 */
	else if (Matches("CREATE", "RULE", MatchAny, "AS", "ON") ||
			 Matches("CREATE", "OR", "REPLACE", "RULE", MatchAny, "AS", "ON"))
		COMPLETE_WITH("SELECT", "UPDATE", "INSERT", "DELETE");
	/* Complete "AS ON SELECT|UPDATE|INSERT|DELETE" with a "TO" */
	else if (TailMatches("AS", "ON", "SELECT|UPDATE|INSERT|DELETE"))
		COMPLETE_WITH("TO");
	/* Complete "AS ON <sth> TO" with a table name */
	else if (TailMatches("AS", "ON", "SELECT|UPDATE|INSERT|DELETE", "TO"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

/* CREATE SCHEMA [ <name> ] [ AUTHORIZATION ] */
	else if (Matches("CREATE", "SCHEMA"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_schemas,
								 "AUTHORIZATION");
	else if (Matches("CREATE", "SCHEMA", "AUTHORIZATION") ||
			 Matches("CREATE", "SCHEMA", MatchAny, "AUTHORIZATION"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
								 Keywords_for_list_of_owner_roles);
	else if (Matches("CREATE", "SCHEMA", "AUTHORIZATION", MatchAny) ||
			 Matches("CREATE", "SCHEMA", MatchAny, "AUTHORIZATION", MatchAny))
		COMPLETE_WITH("CREATE", "GRANT");
	else if (Matches("CREATE", "SCHEMA", MatchAny))
		COMPLETE_WITH("AUTHORIZATION", "CREATE", "GRANT");

/* CREATE SEQUENCE --- is allowed inside CREATE SCHEMA, so use TailMatches */
	else if (TailMatches("CREATE", "SEQUENCE", MatchAny) ||
			 TailMatches("CREATE", "TEMP|TEMPORARY", "SEQUENCE", MatchAny))
		COMPLETE_WITH("AS", "INCREMENT BY", "MINVALUE", "MAXVALUE", "NO",
					  "CACHE", "CYCLE", "OWNED BY", "START WITH");
	else if (TailMatches("CREATE", "SEQUENCE", MatchAny, "AS") ||
			 TailMatches("CREATE", "TEMP|TEMPORARY", "SEQUENCE", MatchAny, "AS"))
		COMPLETE_WITH_CS("smallint", "integer", "bigint");
	else if (TailMatches("CREATE", "SEQUENCE", MatchAny, "NO") ||
			 TailMatches("CREATE", "TEMP|TEMPORARY", "SEQUENCE", MatchAny, "NO"))
		COMPLETE_WITH("MINVALUE", "MAXVALUE", "CYCLE");

/* CREATE SERVER <name> */
	else if (Matches("CREATE", "SERVER", MatchAny))
		COMPLETE_WITH("TYPE", "VERSION", "FOREIGN DATA WRAPPER");

/* CREATE STATISTICS <name> */
	else if (Matches("CREATE", "STATISTICS", MatchAny))
		COMPLETE_WITH("(", "ON");
	else if (Matches("CREATE", "STATISTICS", MatchAny, "("))
		COMPLETE_WITH("ndistinct", "dependencies", "mcv");
	else if (Matches("CREATE", "STATISTICS", MatchAny, "(*)"))
		COMPLETE_WITH("ON");
	else if (HeadMatches("CREATE", "STATISTICS", MatchAny) &&
			 TailMatches("FROM"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

/* CREATE TABLE --- is allowed inside CREATE SCHEMA, so use TailMatches */
	/* Complete "CREATE TEMP/TEMPORARY" with the possible temp objects */
	else if (TailMatches("CREATE", "TEMP|TEMPORARY"))
		COMPLETE_WITH("SEQUENCE", "TABLE", "VIEW");
	/* Complete "CREATE UNLOGGED" with TABLE or SEQUENCE */
	else if (TailMatches("CREATE", "UNLOGGED"))
		COMPLETE_WITH("TABLE", "SEQUENCE");
	/* Complete PARTITION BY with RANGE ( or LIST ( or ... */
	else if (TailMatches("PARTITION", "BY"))
		COMPLETE_WITH("RANGE (", "LIST (", "HASH (");
	/* If we have xxx PARTITION OF, provide a list of partitioned tables */
	else if (TailMatches("PARTITION", "OF"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_partitioned_tables);
	/* Limited completion support for partition bound specification */
	else if (TailMatches("PARTITION", "OF", MatchAny))
		COMPLETE_WITH("FOR VALUES", "DEFAULT");
	/* Complete CREATE TABLE <name> with '(', AS, OF or PARTITION OF */
	else if (TailMatches("CREATE", "TABLE", MatchAny) ||
			 TailMatches("CREATE", "TEMP|TEMPORARY|UNLOGGED", "TABLE", MatchAny))
		COMPLETE_WITH("(", "AS", "OF", "PARTITION OF");
	/* Complete CREATE TABLE <name> OF with list of composite types */
	else if (TailMatches("CREATE", "TABLE", MatchAny, "OF") ||
			 TailMatches("CREATE", "TEMP|TEMPORARY|UNLOGGED", "TABLE", MatchAny, "OF"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_composite_datatypes);
	/* Complete CREATE TABLE <name> [ (...) ] AS with list of keywords */
	else if (TailMatches("CREATE", "TABLE", MatchAny, "AS") ||
			 TailMatches("CREATE", "TABLE", MatchAny, "(*)", "AS") ||
			 TailMatches("CREATE", "TEMP|TEMPORARY|UNLOGGED", "TABLE", MatchAny, "AS") ||
			 TailMatches("CREATE", "TEMP|TEMPORARY|UNLOGGED", "TABLE", MatchAny, "(*)", "AS"))
		COMPLETE_WITH("EXECUTE", "SELECT", "TABLE", "VALUES", "WITH");
	/* Complete CREATE TABLE name (...) with supported options */
	else if (TailMatches("CREATE", "TABLE", MatchAny, "(*)") ||
			 TailMatches("CREATE", "UNLOGGED", "TABLE", MatchAny, "(*)"))
		COMPLETE_WITH("AS", "INHERITS (", "PARTITION BY", "USING", "TABLESPACE", "WITH (");
	else if (TailMatches("CREATE", "TEMP|TEMPORARY", "TABLE", MatchAny, "(*)"))
		COMPLETE_WITH("AS", "INHERITS (", "ON COMMIT", "PARTITION BY",
					  "TABLESPACE", "WITH (");
	/* Complete CREATE TABLE (...) USING with table access methods */
	else if (TailMatches("CREATE", "TABLE", MatchAny, "(*)", "USING") ||
			 TailMatches("CREATE", "TEMP|TEMPORARY|UNLOGGED", "TABLE", MatchAny, "(*)", "USING"))
		COMPLETE_WITH_QUERY(Query_for_list_of_table_access_methods);
	/* Complete CREATE TABLE (...) WITH with storage parameters */
	else if (TailMatches("CREATE", "TABLE", MatchAny, "(*)", "WITH", "(") ||
			 TailMatches("CREATE", "TEMP|TEMPORARY|UNLOGGED", "TABLE", MatchAny, "(*)", "WITH", "("))
		COMPLETE_WITH_LIST(table_storage_parameters);
	/* Complete CREATE TABLE ON COMMIT with actions */
	else if (TailMatches("CREATE", "TEMP|TEMPORARY", "TABLE", MatchAny, "(*)", "ON", "COMMIT"))
		COMPLETE_WITH("DELETE ROWS", "DROP", "PRESERVE ROWS");

/* CREATE TABLESPACE */
	else if (Matches("CREATE", "TABLESPACE", MatchAny))
		COMPLETE_WITH("OWNER", "LOCATION");
	/* Complete CREATE TABLESPACE name OWNER name with "LOCATION" */
	else if (Matches("CREATE", "TABLESPACE", MatchAny, "OWNER", MatchAny))
		COMPLETE_WITH("LOCATION");

/* CREATE TEXT SEARCH */
	else if (Matches("CREATE", "TEXT", "SEARCH"))
		COMPLETE_WITH("CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE");
	else if (Matches("CREATE", "TEXT", "SEARCH", "CONFIGURATION|DICTIONARY|PARSER|TEMPLATE", MatchAny))
		COMPLETE_WITH("(");

/* CREATE TRANSFORM */
	else if (Matches("CREATE", "TRANSFORM") ||
			 Matches("CREATE", "OR", "REPLACE", "TRANSFORM"))
		COMPLETE_WITH("FOR");
	else if (Matches("CREATE", "TRANSFORM", "FOR") ||
			 Matches("CREATE", "OR", "REPLACE", "TRANSFORM", "FOR"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
	else if (Matches("CREATE", "TRANSFORM", "FOR", MatchAny) ||
			 Matches("CREATE", "OR", "REPLACE", "TRANSFORM", "FOR", MatchAny))
		COMPLETE_WITH("LANGUAGE");
	else if (Matches("CREATE", "TRANSFORM", "FOR", MatchAny, "LANGUAGE") ||
			 Matches("CREATE", "OR", "REPLACE", "TRANSFORM", "FOR", MatchAny, "LANGUAGE"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_QUERY(Query_for_list_of_languages);
	}

/* CREATE SUBSCRIPTION */
	else if (Matches("CREATE", "SUBSCRIPTION", MatchAny))
		COMPLETE_WITH("CONNECTION");
	else if (Matches("CREATE", "SUBSCRIPTION", MatchAny, "CONNECTION", MatchAny))
		COMPLETE_WITH("PUBLICATION");
	else if (Matches("CREATE", "SUBSCRIPTION", MatchAny, "CONNECTION",
					 MatchAny, "PUBLICATION"))
	{
		/* complete with nothing here as this refers to remote publications */
	}
	else if (HeadMatches("CREATE", "SUBSCRIPTION") && TailMatches("PUBLICATION", MatchAny))
		COMPLETE_WITH("WITH (");
	/* Complete "CREATE SUBSCRIPTION <name> ...  WITH ( <opt>" */
	else if (HeadMatches("CREATE", "SUBSCRIPTION") && TailMatches("WITH", "("))
		COMPLETE_WITH("binary", "connect", "copy_data", "create_slot",
					  "disable_on_error", "enabled", "failover", "origin",
					  "password_required", "run_as_owner", "slot_name",
					  "streaming", "synchronous_commit", "two_phase");

/* CREATE TRIGGER --- is allowed inside CREATE SCHEMA, so use TailMatches */

	/*
	 * Complete CREATE [ OR REPLACE ] TRIGGER <name> with BEFORE|AFTER|INSTEAD
	 * OF.
	 */
	else if (TailMatches("CREATE", "TRIGGER", MatchAny) ||
			 TailMatches("CREATE", "OR", "REPLACE", "TRIGGER", MatchAny))
		COMPLETE_WITH("BEFORE", "AFTER", "INSTEAD OF");

	/*
	 * Complete CREATE [ OR REPLACE ] TRIGGER <name> BEFORE,AFTER with an
	 * event.
	 */
	else if (TailMatches("CREATE", "TRIGGER", MatchAny, "BEFORE|AFTER") ||
			 TailMatches("CREATE", "OR", "REPLACE", "TRIGGER", MatchAny, "BEFORE|AFTER"))
		COMPLETE_WITH("INSERT", "DELETE", "UPDATE", "TRUNCATE");
	/* Complete CREATE [ OR REPLACE ] TRIGGER <name> INSTEAD OF with an event */
	else if (TailMatches("CREATE", "TRIGGER", MatchAny, "INSTEAD", "OF") ||
			 TailMatches("CREATE", "OR", "REPLACE", "TRIGGER", MatchAny, "INSTEAD", "OF"))
		COMPLETE_WITH("INSERT", "DELETE", "UPDATE");

	/*
	 * Complete CREATE [ OR REPLACE ] TRIGGER <name> BEFORE,AFTER sth with
	 * OR|ON.
	 */
	else if (TailMatches("CREATE", "TRIGGER", MatchAny, "BEFORE|AFTER", MatchAny) ||
			 TailMatches("CREATE", "OR", "REPLACE", "TRIGGER", MatchAny, "BEFORE|AFTER", MatchAny) ||
			 TailMatches("CREATE", "TRIGGER", MatchAny, "INSTEAD", "OF", MatchAny) ||
			 TailMatches("CREATE", "OR", "REPLACE", "TRIGGER", MatchAny, "INSTEAD", "OF", MatchAny))
		COMPLETE_WITH("ON", "OR");

	/*
	 * Complete CREATE [ OR REPLACE ] TRIGGER <name> BEFORE,AFTER event ON
	 * with a list of tables.  EXECUTE FUNCTION is the recommended grammar
	 * instead of EXECUTE PROCEDURE in version 11 and upwards.
	 */
	else if (TailMatches("CREATE", "TRIGGER", MatchAny, "BEFORE|AFTER", MatchAny, "ON") ||
			 TailMatches("CREATE", "OR", "REPLACE", "TRIGGER", MatchAny, "BEFORE|AFTER", MatchAny, "ON"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

	/*
	 * Complete CREATE [ OR REPLACE ] TRIGGER ... INSTEAD OF event ON with a
	 * list of views.
	 */
	else if (TailMatches("CREATE", "TRIGGER", MatchAny, "INSTEAD", "OF", MatchAny, "ON") ||
			 TailMatches("CREATE", "OR", "REPLACE", "TRIGGER", MatchAny, "INSTEAD", "OF", MatchAny, "ON"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views);
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 TailMatches("ON", MatchAny))
	{
		if (pset.sversion >= 110000)
			COMPLETE_WITH("NOT DEFERRABLE", "DEFERRABLE", "INITIALLY",
						  "REFERENCING", "FOR", "WHEN (", "EXECUTE FUNCTION");
		else
			COMPLETE_WITH("NOT DEFERRABLE", "DEFERRABLE", "INITIALLY",
						  "REFERENCING", "FOR", "WHEN (", "EXECUTE PROCEDURE");
	}
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 (TailMatches("DEFERRABLE") || TailMatches("INITIALLY", "IMMEDIATE|DEFERRED")))
	{
		if (pset.sversion >= 110000)
			COMPLETE_WITH("REFERENCING", "FOR", "WHEN (", "EXECUTE FUNCTION");
		else
			COMPLETE_WITH("REFERENCING", "FOR", "WHEN (", "EXECUTE PROCEDURE");
	}
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 TailMatches("REFERENCING"))
		COMPLETE_WITH("OLD TABLE", "NEW TABLE");
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 TailMatches("OLD|NEW", "TABLE"))
		COMPLETE_WITH("AS");
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 (TailMatches("REFERENCING", "OLD", "TABLE", "AS", MatchAny) ||
			  TailMatches("REFERENCING", "OLD", "TABLE", MatchAny)))
	{
		if (pset.sversion >= 110000)
			COMPLETE_WITH("NEW TABLE", "FOR", "WHEN (", "EXECUTE FUNCTION");
		else
			COMPLETE_WITH("NEW TABLE", "FOR", "WHEN (", "EXECUTE PROCEDURE");
	}
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 (TailMatches("REFERENCING", "NEW", "TABLE", "AS", MatchAny) ||
			  TailMatches("REFERENCING", "NEW", "TABLE", MatchAny)))
	{
		if (pset.sversion >= 110000)
			COMPLETE_WITH("OLD TABLE", "FOR", "WHEN (", "EXECUTE FUNCTION");
		else
			COMPLETE_WITH("OLD TABLE", "FOR", "WHEN (", "EXECUTE PROCEDURE");
	}
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 (TailMatches("REFERENCING", "OLD|NEW", "TABLE", "AS", MatchAny, "OLD|NEW", "TABLE", "AS", MatchAny) ||
			  TailMatches("REFERENCING", "OLD|NEW", "TABLE", MatchAny, "OLD|NEW", "TABLE", "AS", MatchAny) ||
			  TailMatches("REFERENCING", "OLD|NEW", "TABLE", "AS", MatchAny, "OLD|NEW", "TABLE", MatchAny) ||
			  TailMatches("REFERENCING", "OLD|NEW", "TABLE", MatchAny, "OLD|NEW", "TABLE", MatchAny)))
	{
		if (pset.sversion >= 110000)
			COMPLETE_WITH("FOR", "WHEN (", "EXECUTE FUNCTION");
		else
			COMPLETE_WITH("FOR", "WHEN (", "EXECUTE PROCEDURE");
	}
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 TailMatches("FOR"))
		COMPLETE_WITH("EACH", "ROW", "STATEMENT");
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 TailMatches("FOR", "EACH"))
		COMPLETE_WITH("ROW", "STATEMENT");
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 (TailMatches("FOR", "EACH", "ROW|STATEMENT") ||
			  TailMatches("FOR", "ROW|STATEMENT")))
	{
		if (pset.sversion >= 110000)
			COMPLETE_WITH("WHEN (", "EXECUTE FUNCTION");
		else
			COMPLETE_WITH("WHEN (", "EXECUTE PROCEDURE");
	}
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 TailMatches("WHEN", "(*)"))
	{
		if (pset.sversion >= 110000)
			COMPLETE_WITH("EXECUTE FUNCTION");
		else
			COMPLETE_WITH("EXECUTE PROCEDURE");
	}

	/*
	 * Complete CREATE [ OR REPLACE ] TRIGGER ... EXECUTE with
	 * PROCEDURE|FUNCTION.
	 */
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 TailMatches("EXECUTE"))
	{
		if (pset.sversion >= 110000)
			COMPLETE_WITH("FUNCTION");
		else
			COMPLETE_WITH("PROCEDURE");
	}
	else if ((HeadMatches("CREATE", "TRIGGER") ||
			  HeadMatches("CREATE", "OR", "REPLACE", "TRIGGER")) &&
			 TailMatches("EXECUTE", "FUNCTION|PROCEDURE"))
		COMPLETE_WITH_VERSIONED_SCHEMA_QUERY(Query_for_list_of_functions);

/* CREATE ROLE,USER,GROUP <name> */
	else if (Matches("CREATE", "ROLE|GROUP|USER", MatchAny) &&
			 !TailMatches("USER", "MAPPING"))
		COMPLETE_WITH("ADMIN", "BYPASSRLS", "CONNECTION LIMIT", "CREATEDB",
					  "CREATEROLE", "ENCRYPTED PASSWORD", "IN", "INHERIT",
					  "LOGIN", "NOBYPASSRLS",
					  "NOCREATEDB", "NOCREATEROLE", "NOINHERIT",
					  "NOLOGIN", "NOREPLICATION", "NOSUPERUSER", "PASSWORD",
					  "REPLICATION", "ROLE", "SUPERUSER", "SYSID",
					  "VALID UNTIL", "WITH");

/* CREATE ROLE,USER,GROUP <name> WITH */
	else if (Matches("CREATE", "ROLE|GROUP|USER", MatchAny, "WITH"))
		/* Similar to the above, but don't complete "WITH" again. */
		COMPLETE_WITH("ADMIN", "BYPASSRLS", "CONNECTION LIMIT", "CREATEDB",
					  "CREATEROLE", "ENCRYPTED PASSWORD", "IN", "INHERIT",
					  "LOGIN", "NOBYPASSRLS",
					  "NOCREATEDB", "NOCREATEROLE", "NOINHERIT",
					  "NOLOGIN", "NOREPLICATION", "NOSUPERUSER", "PASSWORD",
					  "REPLICATION", "ROLE", "SUPERUSER", "SYSID",
					  "VALID UNTIL");

	/* complete CREATE ROLE,USER,GROUP <name> IN with ROLE,GROUP */
	else if (Matches("CREATE", "ROLE|USER|GROUP", MatchAny, "IN"))
		COMPLETE_WITH("GROUP", "ROLE");

/* CREATE TYPE */
	else if (Matches("CREATE", "TYPE", MatchAny))
		COMPLETE_WITH("(", "AS");
	else if (Matches("CREATE", "TYPE", MatchAny, "AS"))
		COMPLETE_WITH("ENUM", "RANGE", "(");
	else if (HeadMatches("CREATE", "TYPE", MatchAny, "AS", "("))
	{
		if (TailMatches("(|*,", MatchAny))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
		else if (TailMatches("(|*,", MatchAny, MatchAnyExcept("*)")))
			COMPLETE_WITH("COLLATE", ",", ")");
	}
	else if (Matches("CREATE", "TYPE", MatchAny, "AS", "ENUM|RANGE"))
		COMPLETE_WITH("(");
	else if (HeadMatches("CREATE", "TYPE", MatchAny, "("))
	{
		if (TailMatches("(|*,"))
			COMPLETE_WITH("INPUT", "OUTPUT", "RECEIVE", "SEND",
						  "TYPMOD_IN", "TYPMOD_OUT", "ANALYZE", "SUBSCRIPT",
						  "INTERNALLENGTH", "PASSEDBYVALUE", "ALIGNMENT",
						  "STORAGE", "LIKE", "CATEGORY", "PREFERRED",
						  "DEFAULT", "ELEMENT", "DELIMITER",
						  "COLLATABLE");
		else if (TailMatches("(*|*,", MatchAnyExcept("*=")))
			COMPLETE_WITH("=");
		else if (TailMatches("=", MatchAnyExcept("*)")))
			COMPLETE_WITH(",", ")");
	}
	else if (HeadMatches("CREATE", "TYPE", MatchAny, "AS", "RANGE", "("))
	{
		if (TailMatches("(|*,"))
			COMPLETE_WITH("SUBTYPE", "SUBTYPE_OPCLASS", "COLLATION",
						  "CANONICAL", "SUBTYPE_DIFF",
						  "MULTIRANGE_TYPE_NAME");
		else if (TailMatches("(*|*,", MatchAnyExcept("*=")))
			COMPLETE_WITH("=");
		else if (TailMatches("=", MatchAnyExcept("*)")))
			COMPLETE_WITH(",", ")");
	}

/* CREATE VIEW --- is allowed inside CREATE SCHEMA, so use TailMatches */
	/* Complete CREATE [ OR REPLACE ] VIEW <name> with AS or WITH */
	else if (TailMatches("CREATE", "VIEW", MatchAny) ||
			 TailMatches("CREATE", "OR", "REPLACE", "VIEW", MatchAny))
		COMPLETE_WITH("AS", "WITH");
	/* Complete "CREATE [ OR REPLACE ] VIEW <sth> AS with "SELECT" */
	else if (TailMatches("CREATE", "VIEW", MatchAny, "AS") ||
			 TailMatches("CREATE", "OR", "REPLACE", "VIEW", MatchAny, "AS"))
		COMPLETE_WITH("SELECT");
	/* CREATE [ OR REPLACE ] VIEW <name> WITH ( yyy [= zzz] ) */
	else if (TailMatches("CREATE", "VIEW", MatchAny, "WITH") ||
			 TailMatches("CREATE", "OR", "REPLACE", "VIEW", MatchAny, "WITH"))
		COMPLETE_WITH("(");
	else if (TailMatches("CREATE", "VIEW", MatchAny, "WITH", "(") ||
			 TailMatches("CREATE", "OR", "REPLACE", "VIEW", MatchAny, "WITH", "("))
		COMPLETE_WITH_LIST(view_optional_parameters);
	else if (TailMatches("CREATE", "VIEW", MatchAny, "WITH", "(", "check_option") ||
			 TailMatches("CREATE", "OR", "REPLACE", "VIEW", MatchAny, "WITH", "(", "check_option"))
		COMPLETE_WITH("=");
	else if (TailMatches("CREATE", "VIEW", MatchAny, "WITH", "(", "check_option", "=") ||
			 TailMatches("CREATE", "OR", "REPLACE", "VIEW", MatchAny, "WITH", "(", "check_option", "="))
		COMPLETE_WITH("local", "cascaded");
	/* CREATE [ OR REPLACE ] VIEW <name> WITH ( ... ) AS */
	else if (TailMatches("CREATE", "VIEW", MatchAny, "WITH", "(*)") ||
			 TailMatches("CREATE", "OR", "REPLACE", "VIEW", MatchAny, "WITH", "(*)"))
		COMPLETE_WITH("AS");
	/* CREATE [ OR REPLACE ] VIEW <name> WITH ( ... ) AS SELECT */
	else if (TailMatches("CREATE", "VIEW", MatchAny, "WITH", "(*)", "AS") ||
			 TailMatches("CREATE", "OR", "REPLACE", "VIEW", MatchAny, "WITH", "(*)", "AS"))
		COMPLETE_WITH("SELECT");

/* CREATE MATERIALIZED VIEW */
	else if (Matches("CREATE", "MATERIALIZED"))
		COMPLETE_WITH("VIEW");
	/* Complete CREATE MATERIALIZED VIEW <name> with AS */
	else if (Matches("CREATE", "MATERIALIZED", "VIEW", MatchAny))
		COMPLETE_WITH("AS");
	/* Complete "CREATE MATERIALIZED VIEW <sth> AS with "SELECT" */
	else if (Matches("CREATE", "MATERIALIZED", "VIEW", MatchAny, "AS"))
		COMPLETE_WITH("SELECT");

/* CREATE EVENT TRIGGER */
	else if (Matches("CREATE", "EVENT"))
		COMPLETE_WITH("TRIGGER");
	/* Complete CREATE EVENT TRIGGER <name> with ON */
	else if (Matches("CREATE", "EVENT", "TRIGGER", MatchAny))
		COMPLETE_WITH("ON");
	/* Complete CREATE EVENT TRIGGER <name> ON with event_type */
	else if (Matches("CREATE", "EVENT", "TRIGGER", MatchAny, "ON"))
		COMPLETE_WITH("ddl_command_start", "ddl_command_end", "login",
					  "sql_drop", "table_rewrite");

	/*
	 * Complete CREATE EVENT TRIGGER <name> ON <event_type>.  EXECUTE FUNCTION
	 * is the recommended grammar instead of EXECUTE PROCEDURE in version 11
	 * and upwards.
	 */
	else if (Matches("CREATE", "EVENT", "TRIGGER", MatchAny, "ON", MatchAny))
	{
		if (pset.sversion >= 110000)
			COMPLETE_WITH("WHEN TAG IN (", "EXECUTE FUNCTION");
		else
			COMPLETE_WITH("WHEN TAG IN (", "EXECUTE PROCEDURE");
	}
	else if (HeadMatches("CREATE", "EVENT", "TRIGGER") &&
			 TailMatches("WHEN|AND", MatchAny, "IN", "(*)"))
	{
		if (pset.sversion >= 110000)
			COMPLETE_WITH("EXECUTE FUNCTION");
		else
			COMPLETE_WITH("EXECUTE PROCEDURE");
	}
	else if (HeadMatches("CREATE", "EVENT", "TRIGGER") &&
			 TailMatches("EXECUTE", "FUNCTION|PROCEDURE"))
		COMPLETE_WITH_VERSIONED_SCHEMA_QUERY(Query_for_list_of_functions);

/* DEALLOCATE */
	else if (Matches("DEALLOCATE"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_prepared_statements,
								 "ALL");

/* DECLARE */

	/*
	 * Complete DECLARE <name> with one of BINARY, ASENSITIVE, INSENSITIVE,
	 * SCROLL, NO SCROLL, and CURSOR.
	 */
	else if (Matches("DECLARE", MatchAny))
		COMPLETE_WITH("BINARY", "ASENSITIVE", "INSENSITIVE", "SCROLL", "NO SCROLL",
					  "CURSOR");

	/*
	 * Complete DECLARE ... <option> with other options. The PostgreSQL parser
	 * allows DECLARE options to be specified in any order. But the
	 * tab-completion follows the ordering of them that the SQL standard
	 * provides, like the syntax of DECLARE command in the documentation
	 * indicates.
	 */
	else if (HeadMatches("DECLARE") && TailMatches("BINARY"))
		COMPLETE_WITH("ASENSITIVE", "INSENSITIVE", "SCROLL", "NO SCROLL", "CURSOR");
	else if (HeadMatches("DECLARE") && TailMatches("ASENSITIVE|INSENSITIVE"))
		COMPLETE_WITH("SCROLL", "NO SCROLL", "CURSOR");
	else if (HeadMatches("DECLARE") && TailMatches("SCROLL"))
		COMPLETE_WITH("CURSOR");
	/* Complete DECLARE ... [options] NO with SCROLL */
	else if (HeadMatches("DECLARE") && TailMatches("NO"))
		COMPLETE_WITH("SCROLL");

	/*
	 * Complete DECLARE ... CURSOR with one of WITH HOLD, WITHOUT HOLD, and
	 * FOR
	 */
	else if (HeadMatches("DECLARE") && TailMatches("CURSOR"))
		COMPLETE_WITH("WITH HOLD", "WITHOUT HOLD", "FOR");
	/* Complete DECLARE ... CURSOR WITH|WITHOUT with HOLD */
	else if (HeadMatches("DECLARE") && TailMatches("CURSOR", "WITH|WITHOUT"))
		COMPLETE_WITH("HOLD");
	/* Complete DECLARE ... CURSOR WITH|WITHOUT HOLD with FOR */
	else if (HeadMatches("DECLARE") && TailMatches("CURSOR", "WITH|WITHOUT", "HOLD"))
		COMPLETE_WITH("FOR");

/* DELETE --- can be inside EXPLAIN, RULE, etc */
	/* Complete DELETE with "FROM" */
	else if (Matches("DELETE"))
		COMPLETE_WITH("FROM");
	/* Complete DELETE FROM with a list of tables */
	else if (TailMatches("DELETE", "FROM"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_updatables);
	/* Complete DELETE FROM <table> */
	else if (TailMatches("DELETE", "FROM", MatchAny))
		COMPLETE_WITH("USING", "WHERE");
	/* XXX: implement tab completion for DELETE ... USING */

/* DISCARD */
	else if (Matches("DISCARD"))
		COMPLETE_WITH("ALL", "PLANS", "SEQUENCES", "TEMP");

/* DO */
	else if (Matches("DO"))
		COMPLETE_WITH("LANGUAGE");

/* DROP */
	/* Complete DROP object with CASCADE / RESTRICT */
	else if (Matches("DROP",
					 "COLLATION|CONVERSION|DOMAIN|EXTENSION|LANGUAGE|PUBLICATION|SCHEMA|SEQUENCE|SERVER|SUBSCRIPTION|STATISTICS|TABLE|TYPE|VIEW",
					 MatchAny) ||
			 Matches("DROP", "ACCESS", "METHOD", MatchAny) ||
			 (Matches("DROP", "AGGREGATE|FUNCTION|PROCEDURE|ROUTINE", MatchAny, MatchAny) &&
			  ends_with(prev_wd, ')')) ||
			 Matches("DROP", "EVENT", "TRIGGER", MatchAny) ||
			 Matches("DROP", "FOREIGN", "DATA", "WRAPPER", MatchAny) ||
			 Matches("DROP", "FOREIGN", "TABLE", MatchAny) ||
			 Matches("DROP", "TEXT", "SEARCH", "CONFIGURATION|DICTIONARY|PARSER|TEMPLATE", MatchAny))
		COMPLETE_WITH("CASCADE", "RESTRICT");

	/* help completing some of the variants */
	else if (Matches("DROP", "AGGREGATE|FUNCTION|PROCEDURE|ROUTINE", MatchAny))
		COMPLETE_WITH("(");
	else if (Matches("DROP", "AGGREGATE|FUNCTION|PROCEDURE|ROUTINE", MatchAny, "("))
		COMPLETE_WITH_FUNCTION_ARG(prev2_wd);
	else if (Matches("DROP", "FOREIGN"))
		COMPLETE_WITH("DATA WRAPPER", "TABLE");
	else if (Matches("DROP", "DATABASE", MatchAny))
		COMPLETE_WITH("WITH (");
	else if (HeadMatches("DROP", "DATABASE") && (ends_with(prev_wd, '(')))
		COMPLETE_WITH("FORCE");

	/* DROP INDEX */
	else if (Matches("DROP", "INDEX"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_indexes,
										"CONCURRENTLY");
	else if (Matches("DROP", "INDEX", "CONCURRENTLY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes);
	else if (Matches("DROP", "INDEX", MatchAny))
		COMPLETE_WITH("CASCADE", "RESTRICT");
	else if (Matches("DROP", "INDEX", "CONCURRENTLY", MatchAny))
		COMPLETE_WITH("CASCADE", "RESTRICT");

	/* DROP MATERIALIZED VIEW */
	else if (Matches("DROP", "MATERIALIZED"))
		COMPLETE_WITH("VIEW");
	else if (Matches("DROP", "MATERIALIZED", "VIEW"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews);
	else if (Matches("DROP", "MATERIALIZED", "VIEW", MatchAny))
		COMPLETE_WITH("CASCADE", "RESTRICT");

	/* DROP OWNED BY */
	else if (Matches("DROP", "OWNED"))
		COMPLETE_WITH("BY");
	else if (Matches("DROP", "OWNED", "BY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (Matches("DROP", "OWNED", "BY", MatchAny))
		COMPLETE_WITH("CASCADE", "RESTRICT");

	/* DROP TEXT SEARCH */
	else if (Matches("DROP", "TEXT", "SEARCH"))
		COMPLETE_WITH("CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE");

	/* DROP TRIGGER */
	else if (Matches("DROP", "TRIGGER", MatchAny))
		COMPLETE_WITH("ON");
	else if (Matches("DROP", "TRIGGER", MatchAny, "ON"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables_for_trigger);
	}
	else if (Matches("DROP", "TRIGGER", MatchAny, "ON", MatchAny))
		COMPLETE_WITH("CASCADE", "RESTRICT");

	/* DROP ACCESS METHOD */
	else if (Matches("DROP", "ACCESS"))
		COMPLETE_WITH("METHOD");
	else if (Matches("DROP", "ACCESS", "METHOD"))
		COMPLETE_WITH_QUERY(Query_for_list_of_access_methods);

	/* DROP EVENT TRIGGER */
	else if (Matches("DROP", "EVENT"))
		COMPLETE_WITH("TRIGGER");
	else if (Matches("DROP", "EVENT", "TRIGGER"))
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);

	/* DROP POLICY <name>  */
	else if (Matches("DROP", "POLICY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_policies);
	/* DROP POLICY <name> ON */
	else if (Matches("DROP", "POLICY", MatchAny))
		COMPLETE_WITH("ON");
	/* DROP POLICY <name> ON <table> */
	else if (Matches("DROP", "POLICY", MatchAny, "ON"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables_for_policy);
	}
	else if (Matches("DROP", "POLICY", MatchAny, "ON", MatchAny))
		COMPLETE_WITH("CASCADE", "RESTRICT");

	/* DROP RULE */
	else if (Matches("DROP", "RULE", MatchAny))
		COMPLETE_WITH("ON");
	else if (Matches("DROP", "RULE", MatchAny, "ON"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables_for_rule);
	}
	else if (Matches("DROP", "RULE", MatchAny, "ON", MatchAny))
		COMPLETE_WITH("CASCADE", "RESTRICT");

	/* DROP TRANSFORM */
	else if (Matches("DROP", "TRANSFORM"))
		COMPLETE_WITH("FOR");
	else if (Matches("DROP", "TRANSFORM", "FOR"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
	else if (Matches("DROP", "TRANSFORM", "FOR", MatchAny))
		COMPLETE_WITH("LANGUAGE");
	else if (Matches("DROP", "TRANSFORM", "FOR", MatchAny, "LANGUAGE"))
	{
		set_completion_reference(prev2_wd);
		COMPLETE_WITH_QUERY(Query_for_list_of_languages);
	}
	else if (Matches("DROP", "TRANSFORM", "FOR", MatchAny, "LANGUAGE", MatchAny))
		COMPLETE_WITH("CASCADE", "RESTRICT");

/* EXECUTE */
	else if (Matches("EXECUTE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_prepared_statements);

/*
 * EXPLAIN [ ( option [, ...] ) ] statement
 * EXPLAIN [ ANALYZE ] [ VERBOSE ] statement
 */
	else if (Matches("EXPLAIN"))
		COMPLETE_WITH("SELECT", "INSERT INTO", "DELETE FROM", "UPDATE", "DECLARE",
					  "MERGE INTO", "EXECUTE", "ANALYZE", "VERBOSE");
	else if (HeadMatches("EXPLAIN", "(*") &&
			 !HeadMatches("EXPLAIN", "(*)"))
	{
		/*
		 * This fires if we're in an unfinished parenthesized option list.
		 * get_previous_words treats a completed parenthesized option list as
		 * one word, so the above test is correct.
		 */
		if (ends_with(prev_wd, '(') || ends_with(prev_wd, ','))
			COMPLETE_WITH("ANALYZE", "VERBOSE", "COSTS", "SETTINGS", "GENERIC_PLAN",
						  "BUFFERS", "SERIALIZE", "WAL", "TIMING", "SUMMARY",
						  "MEMORY", "FORMAT");
		else if (TailMatches("ANALYZE|VERBOSE|COSTS|SETTINGS|GENERIC_PLAN|BUFFERS|WAL|TIMING|SUMMARY|MEMORY"))
			COMPLETE_WITH("ON", "OFF");
		else if (TailMatches("SERIALIZE"))
			COMPLETE_WITH("TEXT", "NONE", "BINARY");
		else if (TailMatches("FORMAT"))
			COMPLETE_WITH("TEXT", "XML", "JSON", "YAML");
	}
	else if (Matches("EXPLAIN", "ANALYZE"))
		COMPLETE_WITH("SELECT", "INSERT INTO", "DELETE FROM", "UPDATE", "DECLARE",
					  "MERGE INTO", "EXECUTE", "VERBOSE");
	else if (Matches("EXPLAIN", "(*)") ||
			 Matches("EXPLAIN", "VERBOSE") ||
			 Matches("EXPLAIN", "ANALYZE", "VERBOSE"))
		COMPLETE_WITH("SELECT", "INSERT INTO", "DELETE FROM", "UPDATE", "DECLARE",
					  "MERGE INTO", "EXECUTE");

/* FETCH && MOVE */

	/*
	 * Complete FETCH with one of ABSOLUTE, BACKWARD, FORWARD, RELATIVE, ALL,
	 * NEXT, PRIOR, FIRST, LAST, FROM, IN, and a list of cursors
	 */
	else if (Matches("FETCH|MOVE"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_cursors,
								 "ABSOLUTE",
								 "BACKWARD",
								 "FORWARD",
								 "RELATIVE",
								 "ALL",
								 "NEXT",
								 "PRIOR",
								 "FIRST",
								 "LAST",
								 "FROM",
								 "IN");

	/*
	 * Complete FETCH BACKWARD or FORWARD with one of ALL, FROM, IN, and a
	 * list of cursors
	 */
	else if (Matches("FETCH|MOVE", "BACKWARD|FORWARD"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_cursors,
								 "ALL",
								 "FROM",
								 "IN");

	/*
	 * Complete FETCH <direction> with "FROM" or "IN". These are equivalent,
	 * but we may as well tab-complete both: perhaps some users prefer one
	 * variant or the other.
	 */
	else if (Matches("FETCH|MOVE", "ABSOLUTE|BACKWARD|FORWARD|RELATIVE",
					 MatchAnyExcept("FROM|IN")) ||
			 Matches("FETCH|MOVE", "ALL|NEXT|PRIOR|FIRST|LAST"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_cursors,
								 "FROM",
								 "IN");
	/* Complete FETCH <direction> "FROM" or "IN" with a list of cursors */
	else if (HeadMatches("FETCH|MOVE") &&
			 TailMatches("FROM|IN"))
		COMPLETE_WITH_QUERY(Query_for_list_of_cursors);

/* FOREIGN DATA WRAPPER */
	/* applies in ALTER/DROP FDW and in CREATE SERVER */
	else if (TailMatches("FOREIGN", "DATA", "WRAPPER") &&
			 !TailMatches("CREATE", MatchAny, MatchAny, MatchAny))
		COMPLETE_WITH_QUERY(Query_for_list_of_fdws);
	/* applies in CREATE SERVER */
	else if (TailMatches("FOREIGN", "DATA", "WRAPPER", MatchAny) &&
			 HeadMatches("CREATE", "SERVER"))
		COMPLETE_WITH("OPTIONS");

/* FOREIGN TABLE */
	else if (TailMatches("FOREIGN", "TABLE") &&
			 !TailMatches("CREATE", MatchAny, MatchAny))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_foreign_tables);

/* FOREIGN SERVER */
	else if (TailMatches("FOREIGN", "SERVER"))
		COMPLETE_WITH_QUERY(Query_for_list_of_servers);

/*
 * GRANT and REVOKE are allowed inside CREATE SCHEMA and
 * ALTER DEFAULT PRIVILEGES, so use TailMatches
 */
	/* Complete GRANT/REVOKE with a list of roles and privileges */
	else if (TailMatches("GRANT|REVOKE") ||
			 TailMatches("REVOKE", "ADMIN|GRANT|INHERIT|SET", "OPTION", "FOR"))
	{
		/*
		 * With ALTER DEFAULT PRIVILEGES, restrict completion to grantable
		 * privileges (can't grant roles)
		 */
		if (HeadMatches("ALTER", "DEFAULT", "PRIVILEGES"))
		{
			if (TailMatches("GRANT") ||
				TailMatches("REVOKE", "GRANT", "OPTION", "FOR"))
				COMPLETE_WITH("SELECT", "INSERT", "UPDATE",
							  "DELETE", "TRUNCATE", "REFERENCES", "TRIGGER",
							  "CREATE", "EXECUTE", "USAGE", "MAINTAIN", "ALL");
			else if (TailMatches("REVOKE"))
				COMPLETE_WITH("SELECT", "INSERT", "UPDATE",
							  "DELETE", "TRUNCATE", "REFERENCES", "TRIGGER",
							  "CREATE", "EXECUTE", "USAGE", "MAINTAIN", "ALL",
							  "GRANT OPTION FOR");
		}
		else if (TailMatches("GRANT"))
			COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
									 Privilege_options_of_grant_and_revoke);
		else if (TailMatches("REVOKE"))
			COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
									 Privilege_options_of_grant_and_revoke,
									 "GRANT OPTION FOR",
									 "ADMIN OPTION FOR",
									 "INHERIT OPTION FOR",
									 "SET OPTION FOR");
		else if (TailMatches("REVOKE", "GRANT", "OPTION", "FOR"))
			COMPLETE_WITH(Privilege_options_of_grant_and_revoke);
		else if (TailMatches("REVOKE", "ADMIN|INHERIT|SET", "OPTION", "FOR"))
			COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	}

	else if (TailMatches("GRANT|REVOKE", "ALTER") ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", "ALTER"))
		COMPLETE_WITH("SYSTEM");

	else if (TailMatches("REVOKE", "SET"))
		COMPLETE_WITH("ON PARAMETER", "OPTION FOR");
	else if (TailMatches("GRANT", "SET") ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", "SET") ||
			 TailMatches("GRANT|REVOKE", "ALTER", "SYSTEM") ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", "ALTER", "SYSTEM"))
		COMPLETE_WITH("ON PARAMETER");

	else if (TailMatches("GRANT|REVOKE", MatchAny, "ON", "PARAMETER") ||
			 TailMatches("GRANT|REVOKE", MatchAny, MatchAny, "ON", "PARAMETER") ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, "ON", "PARAMETER") ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, MatchAny, "ON", "PARAMETER"))
		COMPLETE_WITH_QUERY_VERBATIM(Query_for_list_of_alter_system_set_vars);

	else if (TailMatches("GRANT", MatchAny, "ON", "PARAMETER", MatchAny) ||
			 TailMatches("GRANT", MatchAny, MatchAny, "ON", "PARAMETER", MatchAny))
		COMPLETE_WITH("TO");

	else if (TailMatches("REVOKE", MatchAny, "ON", "PARAMETER", MatchAny) ||
			 TailMatches("REVOKE", MatchAny, MatchAny, "ON", "PARAMETER", MatchAny) ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, "ON", "PARAMETER", MatchAny) ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, MatchAny, "ON", "PARAMETER", MatchAny))
		COMPLETE_WITH("FROM");

	/*
	 * Complete GRANT/REVOKE <privilege> with "ON", GRANT/REVOKE <role> with
	 * TO/FROM
	 */
	else if (TailMatches("GRANT|REVOKE", MatchAny) ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny))
	{
		if (TailMatches("SELECT|INSERT|UPDATE|DELETE|TRUNCATE|REFERENCES|TRIGGER|CREATE|CONNECT|TEMPORARY|TEMP|EXECUTE|USAGE|MAINTAIN|ALL"))
			COMPLETE_WITH("ON");
		else if (TailMatches("GRANT", MatchAny))
			COMPLETE_WITH("TO");
		else
			COMPLETE_WITH("FROM");
	}

	/*
	 * Complete GRANT/REVOKE <sth> ON with a list of appropriate relations.
	 *
	 * Note: GRANT/REVOKE can get quite complex; tab-completion as implemented
	 * here will only work if the privilege list contains exactly one
	 * privilege.
	 */
	else if (TailMatches("GRANT|REVOKE", MatchAny, "ON") ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, "ON"))
	{
		/*
		 * With ALTER DEFAULT PRIVILEGES, restrict completion to the kinds of
		 * objects supported.
		 */
		if (HeadMatches("ALTER", "DEFAULT", "PRIVILEGES"))
			COMPLETE_WITH("TABLES", "SEQUENCES", "FUNCTIONS", "PROCEDURES", "ROUTINES", "TYPES", "SCHEMAS");
		else
			COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_grantables,
											"ALL FUNCTIONS IN SCHEMA",
											"ALL PROCEDURES IN SCHEMA",
											"ALL ROUTINES IN SCHEMA",
											"ALL SEQUENCES IN SCHEMA",
											"ALL TABLES IN SCHEMA",
											"DATABASE",
											"DOMAIN",
											"FOREIGN DATA WRAPPER",
											"FOREIGN SERVER",
											"FUNCTION",
											"LANGUAGE",
											"LARGE OBJECT",
											"PARAMETER",
											"PROCEDURE",
											"ROUTINE",
											"SCHEMA",
											"SEQUENCE",
											"TABLE",
											"TABLESPACE",
											"TYPE");
	}
	else if (TailMatches("GRANT|REVOKE", MatchAny, "ON", "ALL") ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, "ON", "ALL"))
		COMPLETE_WITH("FUNCTIONS IN SCHEMA",
					  "PROCEDURES IN SCHEMA",
					  "ROUTINES IN SCHEMA",
					  "SEQUENCES IN SCHEMA",
					  "TABLES IN SCHEMA");
	else if (TailMatches("GRANT|REVOKE", MatchAny, "ON", "FOREIGN") ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, "ON", "FOREIGN"))
		COMPLETE_WITH("DATA WRAPPER", "SERVER");

	/*
	 * Complete "GRANT/REVOKE * ON DATABASE/DOMAIN/..." with a list of
	 * appropriate objects.
	 *
	 * Complete "GRANT/REVOKE * ON *" with "TO/FROM".
	 */
	else if (TailMatches("GRANT|REVOKE", MatchAny, "ON", MatchAny) ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, "ON", MatchAny))
	{
		if (TailMatches("DATABASE"))
			COMPLETE_WITH_QUERY(Query_for_list_of_databases);
		else if (TailMatches("DOMAIN"))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_domains);
		else if (TailMatches("FUNCTION"))
			COMPLETE_WITH_VERSIONED_SCHEMA_QUERY(Query_for_list_of_functions);
		else if (TailMatches("LANGUAGE"))
			COMPLETE_WITH_QUERY(Query_for_list_of_languages);
		else if (TailMatches("PROCEDURE"))
			COMPLETE_WITH_VERSIONED_SCHEMA_QUERY(Query_for_list_of_procedures);
		else if (TailMatches("ROUTINE"))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_routines);
		else if (TailMatches("SCHEMA"))
			COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
		else if (TailMatches("SEQUENCE"))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_sequences);
		else if (TailMatches("TABLE"))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_grantables);
		else if (TailMatches("TABLESPACE"))
			COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
		else if (TailMatches("TYPE"))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
		else if (TailMatches("GRANT", MatchAny, MatchAny, MatchAny))
			COMPLETE_WITH("TO");
		else
			COMPLETE_WITH("FROM");
	}

	/*
	 * Complete "GRANT/REVOKE ... TO/FROM" with username, PUBLIC,
	 * CURRENT_ROLE, CURRENT_USER, or SESSION_USER.
	 */
	else if ((HeadMatches("GRANT") && TailMatches("TO")) ||
			 (HeadMatches("REVOKE") && TailMatches("FROM")))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
								 Keywords_for_list_of_grant_roles);

	/*
	 * Offer grant options after that.
	 */
	else if (HeadMatches("GRANT") && TailMatches("TO", MatchAny))
		COMPLETE_WITH("WITH ADMIN",
					  "WITH INHERIT",
					  "WITH SET",
					  "WITH GRANT OPTION",
					  "GRANTED BY");
	else if (HeadMatches("GRANT") && TailMatches("TO", MatchAny, "WITH"))
		COMPLETE_WITH("ADMIN",
					  "INHERIT",
					  "SET",
					  "GRANT OPTION");
	else if (HeadMatches("GRANT") &&
			 (TailMatches("TO", MatchAny, "WITH", "ADMIN|INHERIT|SET")))
		COMPLETE_WITH("OPTION", "TRUE", "FALSE");
	else if (HeadMatches("GRANT") && TailMatches("TO", MatchAny, "WITH", MatchAny, "OPTION"))
		COMPLETE_WITH("GRANTED BY");
	else if (HeadMatches("GRANT") && TailMatches("TO", MatchAny, "WITH", MatchAny, "OPTION", "GRANTED", "BY"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
								 Keywords_for_list_of_grant_roles);
	/* Complete "ALTER DEFAULT PRIVILEGES ... GRANT/REVOKE ... TO/FROM */
	else if (HeadMatches("ALTER", "DEFAULT", "PRIVILEGES") && TailMatches("TO|FROM"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
								 Keywords_for_list_of_grant_roles);
	/* Offer WITH GRANT OPTION after that */
	else if (HeadMatches("ALTER", "DEFAULT", "PRIVILEGES") && TailMatches("TO", MatchAny))
		COMPLETE_WITH("WITH GRANT OPTION");
	/* Complete "GRANT/REVOKE ... ON * *" with TO/FROM */
	else if (HeadMatches("GRANT") && TailMatches("ON", MatchAny, MatchAny))
		COMPLETE_WITH("TO");
	else if (HeadMatches("REVOKE") && TailMatches("ON", MatchAny, MatchAny))
		COMPLETE_WITH("FROM");

	/* Complete "GRANT/REVOKE * ON ALL * IN SCHEMA *" with TO/FROM */
	else if (TailMatches("GRANT|REVOKE", MatchAny, "ON", "ALL", MatchAny, "IN", "SCHEMA", MatchAny) ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, "ON", "ALL", MatchAny, "IN", "SCHEMA", MatchAny))
	{
		if (TailMatches("GRANT", MatchAny, MatchAny, MatchAny, MatchAny, MatchAny, MatchAny, MatchAny))
			COMPLETE_WITH("TO");
		else
			COMPLETE_WITH("FROM");
	}

	/* Complete "GRANT/REVOKE * ON FOREIGN DATA WRAPPER *" with TO/FROM */
	else if (TailMatches("GRANT|REVOKE", MatchAny, "ON", "FOREIGN", "DATA", "WRAPPER", MatchAny) ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, "ON", "FOREIGN", "DATA", "WRAPPER", MatchAny))
	{
		if (TailMatches("GRANT", MatchAny, MatchAny, MatchAny, MatchAny, MatchAny, MatchAny))
			COMPLETE_WITH("TO");
		else
			COMPLETE_WITH("FROM");
	}

	/* Complete "GRANT/REVOKE * ON FOREIGN SERVER *" with TO/FROM */
	else if (TailMatches("GRANT|REVOKE", MatchAny, "ON", "FOREIGN", "SERVER", MatchAny) ||
			 TailMatches("REVOKE", "GRANT", "OPTION", "FOR", MatchAny, "ON", "FOREIGN", "SERVER", MatchAny))
	{
		if (TailMatches("GRANT", MatchAny, MatchAny, MatchAny, MatchAny, MatchAny))
			COMPLETE_WITH("TO");
		else
			COMPLETE_WITH("FROM");
	}

/* GROUP BY */
	else if (TailMatches("FROM", MatchAny, "GROUP"))
		COMPLETE_WITH("BY");

/* IMPORT FOREIGN SCHEMA */
	else if (Matches("IMPORT"))
		COMPLETE_WITH("FOREIGN SCHEMA");
	else if (Matches("IMPORT", "FOREIGN"))
		COMPLETE_WITH("SCHEMA");
	else if (Matches("IMPORT", "FOREIGN", "SCHEMA", MatchAny))
		COMPLETE_WITH("EXCEPT (", "FROM SERVER", "LIMIT TO (");
	else if (TailMatches("LIMIT", "TO", "(*)") ||
			 TailMatches("EXCEPT", "(*)"))
		COMPLETE_WITH("FROM SERVER");
	else if (TailMatches("FROM", "SERVER", MatchAny))
		COMPLETE_WITH("INTO");
	else if (TailMatches("FROM", "SERVER", MatchAny, "INTO"))
		COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
	else if (TailMatches("FROM", "SERVER", MatchAny, "INTO", MatchAny))
		COMPLETE_WITH("OPTIONS (");

/* INSERT --- can be inside EXPLAIN, RULE, etc */
	/* Complete NOT MATCHED THEN INSERT */
	else if (TailMatches("NOT", "MATCHED", "THEN", "INSERT"))
		COMPLETE_WITH("VALUES", "(");
	/* Complete INSERT with "INTO" */
	else if (TailMatches("INSERT"))
		COMPLETE_WITH("INTO");
	/* Complete INSERT INTO with table names */
	else if (TailMatches("INSERT", "INTO"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_updatables);
	/* Complete "INSERT INTO <table> (" with attribute names */
	else if (TailMatches("INSERT", "INTO", MatchAny, "("))
		COMPLETE_WITH_ATTR(prev2_wd);

	/*
	 * Complete INSERT INTO <table> with "(" or "VALUES" or "SELECT" or
	 * "TABLE" or "DEFAULT VALUES" or "OVERRIDING"
	 */
	else if (TailMatches("INSERT", "INTO", MatchAny))
		COMPLETE_WITH("(", "DEFAULT VALUES", "SELECT", "TABLE", "VALUES", "OVERRIDING");

	/*
	 * Complete INSERT INTO <table> (attribs) with "VALUES" or "SELECT" or
	 * "TABLE" or "OVERRIDING"
	 */
	else if (TailMatches("INSERT", "INTO", MatchAny, MatchAny) &&
			 ends_with(prev_wd, ')'))
		COMPLETE_WITH("SELECT", "TABLE", "VALUES", "OVERRIDING");

	/* Complete OVERRIDING */
	else if (TailMatches("OVERRIDING"))
		COMPLETE_WITH("SYSTEM VALUE", "USER VALUE");

	/* Complete after OVERRIDING clause */
	else if (TailMatches("OVERRIDING", MatchAny, "VALUE"))
		COMPLETE_WITH("SELECT", "TABLE", "VALUES");

	/* Insert an open parenthesis after "VALUES" */
	else if (TailMatches("VALUES") && !TailMatches("DEFAULT", "VALUES"))
		COMPLETE_WITH("(");

/* LOCK */
	/* Complete LOCK [TABLE] [ONLY] with a list of tables */
	else if (Matches("LOCK"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_tables,
										"TABLE", "ONLY");
	else if (Matches("LOCK", "TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_tables,
										"ONLY");
	else if (Matches("LOCK", "TABLE", "ONLY") || Matches("LOCK", "ONLY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	/* For the following, handle the case of a single table only for now */

	/* Complete LOCK [TABLE] [ONLY] <table> with IN or NOWAIT */
	else if (Matches("LOCK", MatchAnyExcept("TABLE|ONLY")) ||
			 Matches("LOCK", "TABLE", MatchAnyExcept("ONLY")) ||
			 Matches("LOCK", "ONLY", MatchAny) ||
			 Matches("LOCK", "TABLE", "ONLY", MatchAny))
		COMPLETE_WITH("IN", "NOWAIT");

	/* Complete LOCK [TABLE] [ONLY] <table> IN with a lock mode */
	else if (HeadMatches("LOCK") && TailMatches("IN"))
		COMPLETE_WITH("ACCESS SHARE MODE",
					  "ROW SHARE MODE", "ROW EXCLUSIVE MODE",
					  "SHARE UPDATE EXCLUSIVE MODE", "SHARE MODE",
					  "SHARE ROW EXCLUSIVE MODE",
					  "EXCLUSIVE MODE", "ACCESS EXCLUSIVE MODE");

	/*
	 * Complete LOCK [TABLE][ONLY] <table> IN ACCESS|ROW with rest of lock
	 * mode
	 */
	else if (HeadMatches("LOCK") && TailMatches("IN", "ACCESS|ROW"))
		COMPLETE_WITH("EXCLUSIVE MODE", "SHARE MODE");

	/* Complete LOCK [TABLE] [ONLY] <table> IN SHARE with rest of lock mode */
	else if (HeadMatches("LOCK") && TailMatches("IN", "SHARE"))
		COMPLETE_WITH("MODE", "ROW EXCLUSIVE MODE",
					  "UPDATE EXCLUSIVE MODE");

	/* Complete LOCK [TABLE] [ONLY] <table> [IN lockmode MODE] with "NOWAIT" */
	else if (HeadMatches("LOCK") && TailMatches("MODE"))
		COMPLETE_WITH("NOWAIT");

/* MERGE --- can be inside EXPLAIN */
	else if (TailMatches("MERGE"))
		COMPLETE_WITH("INTO");
	else if (TailMatches("MERGE", "INTO"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_mergetargets);

	/* Complete MERGE INTO <table> [[AS] <alias>] with USING */
	else if (TailMatches("MERGE", "INTO", MatchAny))
		COMPLETE_WITH("USING", "AS");
	else if (TailMatches("MERGE", "INTO", MatchAny, "AS", MatchAny) ||
			 TailMatches("MERGE", "INTO", MatchAny, MatchAnyExcept("USING|AS")))
		COMPLETE_WITH("USING");

	/*
	 * Complete MERGE INTO ... USING with a list of relations supporting
	 * SELECT
	 */
	else if (TailMatches("MERGE", "INTO", MatchAny, "USING") ||
			 TailMatches("MERGE", "INTO", MatchAny, "AS", MatchAny, "USING") ||
			 TailMatches("MERGE", "INTO", MatchAny, MatchAny, "USING"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_selectables);

	/*
	 * Complete MERGE INTO <table> [[AS] <alias>] USING <relations> [[AS]
	 * alias] with ON
	 */
	else if (TailMatches("MERGE", "INTO", MatchAny, "USING", MatchAny) ||
			 TailMatches("MERGE", "INTO", MatchAny, "AS", MatchAny, "USING", MatchAny) ||
			 TailMatches("MERGE", "INTO", MatchAny, MatchAny, "USING", MatchAny))
		COMPLETE_WITH("AS", "ON");
	else if (TailMatches("MERGE", "INTO", MatchAny, "USING", MatchAny, "AS", MatchAny) ||
			 TailMatches("MERGE", "INTO", MatchAny, "AS", MatchAny, "USING", MatchAny, "AS", MatchAny) ||
			 TailMatches("MERGE", "INTO", MatchAny, MatchAny, "USING", MatchAny, "AS", MatchAny) ||
			 TailMatches("MERGE", "INTO", MatchAny, "USING", MatchAny, MatchAnyExcept("ON|AS")) ||
			 TailMatches("MERGE", "INTO", MatchAny, "AS", MatchAny, "USING", MatchAny, MatchAnyExcept("ON|AS")) ||
			 TailMatches("MERGE", "INTO", MatchAny, MatchAny, "USING", MatchAny, MatchAnyExcept("ON|AS")))
		COMPLETE_WITH("ON");

	/* Complete MERGE INTO ... ON with target table attributes */
	else if (TailMatches("INTO", MatchAny, "USING", MatchAny, "ON"))
		COMPLETE_WITH_ATTR(prev4_wd);
	else if (TailMatches("INTO", MatchAny, "AS", MatchAny, "USING", MatchAny, "AS", MatchAny, "ON"))
		COMPLETE_WITH_ATTR(prev8_wd);
	else if (TailMatches("INTO", MatchAny, MatchAny, "USING", MatchAny, MatchAny, "ON"))
		COMPLETE_WITH_ATTR(prev6_wd);

	/*
	 * Complete ... USING <relation> [[AS] alias] ON join condition
	 * (consisting of one or three words typically used) with WHEN [NOT]
	 * MATCHED
	 */
	else if (TailMatches("USING", MatchAny, "ON", MatchAny) ||
			 TailMatches("USING", MatchAny, "AS", MatchAny, "ON", MatchAny) ||
			 TailMatches("USING", MatchAny, MatchAny, "ON", MatchAny) ||
			 TailMatches("USING", MatchAny, "ON", MatchAny, MatchAnyExcept("WHEN"), MatchAnyExcept("WHEN")) ||
			 TailMatches("USING", MatchAny, "AS", MatchAny, "ON", MatchAny, MatchAnyExcept("WHEN"), MatchAnyExcept("WHEN")) ||
			 TailMatches("USING", MatchAny, MatchAny, "ON", MatchAny, MatchAnyExcept("WHEN"), MatchAnyExcept("WHEN")))
		COMPLETE_WITH("WHEN MATCHED", "WHEN NOT MATCHED");
	else if (TailMatches("USING", MatchAny, "ON", MatchAny, "WHEN") ||
			 TailMatches("USING", MatchAny, "AS", MatchAny, "ON", MatchAny, "WHEN") ||
			 TailMatches("USING", MatchAny, MatchAny, "ON", MatchAny, "WHEN") ||
			 TailMatches("USING", MatchAny, "ON", MatchAny, MatchAny, MatchAny, "WHEN") ||
			 TailMatches("USING", MatchAny, "AS", MatchAny, "ON", MatchAny, MatchAny, MatchAny, "WHEN") ||
			 TailMatches("USING", MatchAny, MatchAny, "ON", MatchAny, MatchAny, MatchAny, "WHEN"))
		COMPLETE_WITH("MATCHED", "NOT MATCHED");

	/*
	 * Complete ... WHEN MATCHED and WHEN NOT MATCHED BY SOURCE|TARGET with
	 * THEN/AND
	 */
	else if (TailMatches("WHEN", "MATCHED") ||
			 TailMatches("WHEN", "NOT", "MATCHED", "BY", "SOURCE|TARGET"))
		COMPLETE_WITH("THEN", "AND");

	/* Complete ... WHEN NOT MATCHED with BY/THEN/AND */
	else if (TailMatches("WHEN", "NOT", "MATCHED"))
		COMPLETE_WITH("BY", "THEN", "AND");

	/* Complete ... WHEN NOT MATCHED BY with SOURCE/TARGET */
	else if (TailMatches("WHEN", "NOT", "MATCHED", "BY"))
		COMPLETE_WITH("SOURCE", "TARGET");

	/*
	 * Complete ... WHEN MATCHED THEN and WHEN NOT MATCHED BY SOURCE THEN with
	 * UPDATE SET/DELETE/DO NOTHING
	 */
	else if (TailMatches("WHEN", "MATCHED", "THEN") ||
			 TailMatches("WHEN", "NOT", "MATCHED", "BY", "SOURCE", "THEN"))
		COMPLETE_WITH("UPDATE SET", "DELETE", "DO NOTHING");

	/*
	 * Complete ... WHEN NOT MATCHED [BY TARGET] THEN with INSERT/DO NOTHING
	 */
	else if (TailMatches("WHEN", "NOT", "MATCHED", "THEN") ||
			 TailMatches("WHEN", "NOT", "MATCHED", "BY", "TARGET", "THEN"))
		COMPLETE_WITH("INSERT", "DO NOTHING");

/* NOTIFY --- can be inside EXPLAIN, RULE, etc */
	else if (TailMatches("NOTIFY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_channels);

/* OPTIONS */
	else if (TailMatches("OPTIONS"))
		COMPLETE_WITH("(");

/* OWNER TO  - complete with available roles */
	else if (TailMatches("OWNER", "TO"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
								 Keywords_for_list_of_owner_roles);

/* ORDER BY */
	else if (TailMatches("FROM", MatchAny, "ORDER"))
		COMPLETE_WITH("BY");
	else if (TailMatches("FROM", MatchAny, "ORDER", "BY"))
		COMPLETE_WITH_ATTR(prev3_wd);

/* PREPARE xx AS */
	else if (Matches("PREPARE", MatchAny, "AS"))
		COMPLETE_WITH("SELECT", "UPDATE", "INSERT INTO", "DELETE FROM");

/*
 * PREPARE TRANSACTION is missing on purpose. It's intended for transaction
 * managers, not for manual use in interactive sessions.
 */

/* REASSIGN OWNED BY xxx TO yyy */
	else if (Matches("REASSIGN"))
		COMPLETE_WITH("OWNED BY");
	else if (Matches("REASSIGN", "OWNED"))
		COMPLETE_WITH("BY");
	else if (Matches("REASSIGN", "OWNED", "BY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (Matches("REASSIGN", "OWNED", "BY", MatchAny))
		COMPLETE_WITH("TO");
	else if (Matches("REASSIGN", "OWNED", "BY", MatchAny, "TO"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);

/* REFRESH MATERIALIZED VIEW */
	else if (Matches("REFRESH"))
		COMPLETE_WITH("MATERIALIZED VIEW");
	else if (Matches("REFRESH", "MATERIALIZED"))
		COMPLETE_WITH("VIEW");
	else if (Matches("REFRESH", "MATERIALIZED", "VIEW"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_matviews,
										"CONCURRENTLY");
	else if (Matches("REFRESH", "MATERIALIZED", "VIEW", "CONCURRENTLY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews);
	else if (Matches("REFRESH", "MATERIALIZED", "VIEW", MatchAny))
		COMPLETE_WITH("WITH");
	else if (Matches("REFRESH", "MATERIALIZED", "VIEW", "CONCURRENTLY", MatchAny))
		COMPLETE_WITH("WITH");
	else if (Matches("REFRESH", "MATERIALIZED", "VIEW", MatchAny, "WITH"))
		COMPLETE_WITH("NO DATA", "DATA");
	else if (Matches("REFRESH", "MATERIALIZED", "VIEW", "CONCURRENTLY", MatchAny, "WITH"))
		COMPLETE_WITH("NO DATA", "DATA");
	else if (Matches("REFRESH", "MATERIALIZED", "VIEW", MatchAny, "WITH", "NO"))
		COMPLETE_WITH("DATA");
	else if (Matches("REFRESH", "MATERIALIZED", "VIEW", "CONCURRENTLY", MatchAny, "WITH", "NO"))
		COMPLETE_WITH("DATA");

/* REINDEX */
	else if (Matches("REINDEX") ||
			 Matches("REINDEX", "(*)"))
		COMPLETE_WITH("TABLE", "INDEX", "SYSTEM", "SCHEMA", "DATABASE");
	else if (Matches("REINDEX", "TABLE") ||
			 Matches("REINDEX", "(*)", "TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_indexables,
										"CONCURRENTLY");
	else if (Matches("REINDEX", "INDEX") ||
			 Matches("REINDEX", "(*)", "INDEX"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_indexes,
										"CONCURRENTLY");
	else if (Matches("REINDEX", "SCHEMA") ||
			 Matches("REINDEX", "(*)", "SCHEMA"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_schemas,
								 "CONCURRENTLY");
	else if (Matches("REINDEX", "SYSTEM|DATABASE") ||
			 Matches("REINDEX", "(*)", "SYSTEM|DATABASE"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_databases,
								 "CONCURRENTLY");
	else if (Matches("REINDEX", "TABLE", "CONCURRENTLY") ||
			 Matches("REINDEX", "(*)", "TABLE", "CONCURRENTLY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexables);
	else if (Matches("REINDEX", "INDEX", "CONCURRENTLY") ||
			 Matches("REINDEX", "(*)", "INDEX", "CONCURRENTLY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes);
	else if (Matches("REINDEX", "SCHEMA", "CONCURRENTLY") ||
			 Matches("REINDEX", "(*)", "SCHEMA", "CONCURRENTLY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
	else if (Matches("REINDEX", "SYSTEM|DATABASE", "CONCURRENTLY") ||
			 Matches("REINDEX", "(*)", "SYSTEM|DATABASE", "CONCURRENTLY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_databases);
	else if (HeadMatches("REINDEX", "(*") &&
			 !HeadMatches("REINDEX", "(*)"))
	{
		/*
		 * This fires if we're in an unfinished parenthesized option list.
		 * get_previous_words treats a completed parenthesized option list as
		 * one word, so the above test is correct.
		 */
		if (ends_with(prev_wd, '(') || ends_with(prev_wd, ','))
			COMPLETE_WITH("CONCURRENTLY", "TABLESPACE", "VERBOSE");
		else if (TailMatches("TABLESPACE"))
			COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
	}

/* SECURITY LABEL */
	else if (Matches("SECURITY"))
		COMPLETE_WITH("LABEL");
	else if (Matches("SECURITY", "LABEL"))
		COMPLETE_WITH("ON", "FOR");
	else if (Matches("SECURITY", "LABEL", "FOR", MatchAny))
		COMPLETE_WITH("ON");
	else if (Matches("SECURITY", "LABEL", "ON") ||
			 Matches("SECURITY", "LABEL", "FOR", MatchAny, "ON"))
		COMPLETE_WITH("TABLE", "COLUMN", "AGGREGATE", "DATABASE", "DOMAIN",
					  "EVENT TRIGGER", "FOREIGN TABLE", "FUNCTION",
					  "LARGE OBJECT", "MATERIALIZED VIEW", "LANGUAGE",
					  "PUBLICATION", "PROCEDURE", "ROLE", "ROUTINE", "SCHEMA",
					  "SEQUENCE", "SUBSCRIPTION", "TABLESPACE", "TYPE", "VIEW");
	else if (Matches("SECURITY", "LABEL", "ON", MatchAny, MatchAny))
		COMPLETE_WITH("IS");

/* SELECT */
	/* naah . . . */

/* SET, RESET, SHOW */
	/* Complete with a variable name */
	else if (TailMatches("SET|RESET") && !TailMatches("UPDATE", MatchAny, "SET"))
		COMPLETE_WITH_QUERY_VERBATIM_PLUS(Query_for_list_of_set_vars,
										  "CONSTRAINTS",
										  "TRANSACTION",
										  "SESSION",
										  "ROLE",
										  "TABLESPACE",
										  "ALL");
	else if (Matches("SHOW"))
		COMPLETE_WITH_QUERY_VERBATIM_PLUS(Query_for_list_of_show_vars,
										  "SESSION AUTHORIZATION",
										  "ALL");
	else if (Matches("SHOW", "SESSION"))
		COMPLETE_WITH("AUTHORIZATION");
	/* Complete "SET TRANSACTION" */
	else if (Matches("SET", "TRANSACTION"))
		COMPLETE_WITH("SNAPSHOT", "ISOLATION LEVEL", "READ", "DEFERRABLE", "NOT DEFERRABLE");
	else if (Matches("BEGIN|START", "TRANSACTION") ||
			 Matches("BEGIN", "WORK") ||
			 Matches("BEGIN") ||
			 Matches("SET", "SESSION", "CHARACTERISTICS", "AS", "TRANSACTION"))
		COMPLETE_WITH("ISOLATION LEVEL", "READ", "DEFERRABLE", "NOT DEFERRABLE");
	else if (Matches("SET|BEGIN|START", "TRANSACTION|WORK", "NOT") ||
			 Matches("BEGIN", "NOT") ||
			 Matches("SET", "SESSION", "CHARACTERISTICS", "AS", "TRANSACTION", "NOT"))
		COMPLETE_WITH("DEFERRABLE");
	else if (Matches("SET|BEGIN|START", "TRANSACTION|WORK", "ISOLATION") ||
			 Matches("BEGIN", "ISOLATION") ||
			 Matches("SET", "SESSION", "CHARACTERISTICS", "AS", "TRANSACTION", "ISOLATION"))
		COMPLETE_WITH("LEVEL");
	else if (Matches("SET|BEGIN|START", "TRANSACTION|WORK", "ISOLATION", "LEVEL") ||
			 Matches("BEGIN", "ISOLATION", "LEVEL") ||
			 Matches("SET", "SESSION", "CHARACTERISTICS", "AS", "TRANSACTION", "ISOLATION", "LEVEL"))
		COMPLETE_WITH("READ", "REPEATABLE READ", "SERIALIZABLE");
	else if (Matches("SET|BEGIN|START", "TRANSACTION|WORK", "ISOLATION", "LEVEL", "READ") ||
			 Matches("BEGIN", "ISOLATION", "LEVEL", "READ") ||
			 Matches("SET", "SESSION", "CHARACTERISTICS", "AS", "TRANSACTION", "ISOLATION", "LEVEL", "READ"))
		COMPLETE_WITH("UNCOMMITTED", "COMMITTED");
	else if (Matches("SET|BEGIN|START", "TRANSACTION|WORK", "ISOLATION", "LEVEL", "REPEATABLE") ||
			 Matches("BEGIN", "ISOLATION", "LEVEL", "REPEATABLE") ||
			 Matches("SET", "SESSION", "CHARACTERISTICS", "AS", "TRANSACTION", "ISOLATION", "LEVEL", "REPEATABLE"))
		COMPLETE_WITH("READ");
	else if (Matches("SET|BEGIN|START", "TRANSACTION|WORK", "READ") ||
			 Matches("BEGIN", "READ") ||
			 Matches("SET", "SESSION", "CHARACTERISTICS", "AS", "TRANSACTION", "READ"))
		COMPLETE_WITH("ONLY", "WRITE");
	/* SET CONSTRAINTS */
	else if (Matches("SET", "CONSTRAINTS"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_constraints_with_schema,
										"ALL");
	/* Complete SET CONSTRAINTS <foo> with DEFERRED|IMMEDIATE */
	else if (Matches("SET", "CONSTRAINTS", MatchAny))
		COMPLETE_WITH("DEFERRED", "IMMEDIATE");
	/* Complete SET ROLE */
	else if (Matches("SET", "ROLE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	/* Complete SET SESSION with AUTHORIZATION or CHARACTERISTICS... */
	else if (Matches("SET", "SESSION"))
		COMPLETE_WITH("AUTHORIZATION", "CHARACTERISTICS AS TRANSACTION");
	/* Complete SET SESSION AUTHORIZATION with username */
	else if (Matches("SET", "SESSION", "AUTHORIZATION"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
								 "DEFAULT");
	/* Complete RESET SESSION with AUTHORIZATION */
	else if (Matches("RESET", "SESSION"))
		COMPLETE_WITH("AUTHORIZATION");
	/* Complete SET <var> with "TO" */
	else if (Matches("SET", MatchAny))
		COMPLETE_WITH("TO");

	/*
	 * Complete ALTER DATABASE|FUNCTION|PROCEDURE|ROLE|ROUTINE|USER ... SET
	 * <name>
	 */
	else if (HeadMatches("ALTER", "DATABASE|FUNCTION|PROCEDURE|ROLE|ROUTINE|USER") &&
			 TailMatches("SET", MatchAny) &&
			 !TailMatches("SCHEMA"))
		COMPLETE_WITH("FROM CURRENT", "TO");

	/*
	 * Suggest possible variable values in SET variable TO|=, along with the
	 * preceding ALTER syntaxes.
	 */
	else if (TailMatches("SET", MatchAny, "TO|=") &&
			 !TailMatches("UPDATE", MatchAny, "SET", MatchAny, "TO|="))
	{
		/* special cased code for individual GUCs */
		if (TailMatches("DateStyle", "TO|="))
			COMPLETE_WITH("ISO", "SQL", "Postgres", "German",
						  "YMD", "DMY", "MDY",
						  "US", "European", "NonEuropean",
						  "DEFAULT");
		else if (TailMatches("search_path", "TO|="))
		{
			/* Here, we want to allow pg_catalog, so use narrower exclusion */
			COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_schemas
									 " AND nspname NOT LIKE E'pg\\\\_toast%%'"
									 " AND nspname NOT LIKE E'pg\\\\_temp%%'",
									 "DEFAULT");
		}
		else if (TailMatches("TimeZone", "TO|="))
			COMPLETE_WITH_TIMEZONE_NAME();
		else
		{
			/* generic, type based, GUC support */
			char	   *guctype = get_guctype(prev2_wd);

			/*
			 * Note: if we don't recognize the GUC name, it's important to not
			 * offer any completions, as most likely we've misinterpreted the
			 * context and this isn't a GUC-setting command at all.
			 */
			if (guctype)
			{
				if (strcmp(guctype, "enum") == 0)
				{
					set_completion_reference_verbatim(prev2_wd);
					COMPLETE_WITH_QUERY_PLUS(Query_for_values_of_enum_GUC,
											 "DEFAULT");
				}
				else if (strcmp(guctype, "bool") == 0)
					COMPLETE_WITH("on", "off", "true", "false", "yes", "no",
								  "1", "0", "DEFAULT");
				else
					COMPLETE_WITH("DEFAULT");

				free(guctype);
			}
		}
	}

/* START TRANSACTION */
	else if (Matches("START"))
		COMPLETE_WITH("TRANSACTION");

/* TABLE, but not TABLE embedded in other commands */
	else if (Matches("TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_selectables);

/* TABLESAMPLE */
	else if (TailMatches("TABLESAMPLE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_tablesample_methods);
	else if (TailMatches("TABLESAMPLE", MatchAny))
		COMPLETE_WITH("(");

/* TRUNCATE */
	else if (Matches("TRUNCATE"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_truncatables,
										"TABLE", "ONLY");
	else if (Matches("TRUNCATE", "TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_truncatables,
										"ONLY");
	else if (HeadMatches("TRUNCATE") && TailMatches("ONLY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_truncatables);
	else if (Matches("TRUNCATE", MatchAny) ||
			 Matches("TRUNCATE", "TABLE|ONLY", MatchAny) ||
			 Matches("TRUNCATE", "TABLE", "ONLY", MatchAny))
		COMPLETE_WITH("RESTART IDENTITY", "CONTINUE IDENTITY", "CASCADE", "RESTRICT");
	else if (HeadMatches("TRUNCATE") && TailMatches("IDENTITY"))
		COMPLETE_WITH("CASCADE", "RESTRICT");

/* UNLISTEN */
	else if (Matches("UNLISTEN"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_channels, "*");

/* UPDATE --- can be inside EXPLAIN, RULE, etc */
	/* If prev. word is UPDATE suggest a list of tables */
	else if (TailMatches("UPDATE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_updatables);
	/* Complete UPDATE <table> with "SET" */
	else if (TailMatches("UPDATE", MatchAny))
		COMPLETE_WITH("SET");
	/* Complete UPDATE <table> SET with list of attributes */
	else if (TailMatches("UPDATE", MatchAny, "SET"))
		COMPLETE_WITH_ATTR(prev2_wd);
	/* UPDATE <table> SET <attr> = */
	else if (TailMatches("UPDATE", MatchAny, "SET", MatchAnyExcept("*=")))
		COMPLETE_WITH("=");

/* USER MAPPING */
	else if (Matches("ALTER|CREATE|DROP", "USER", "MAPPING"))
		COMPLETE_WITH("FOR");
	else if (Matches("CREATE", "USER", "MAPPING", "FOR"))
		COMPLETE_WITH_QUERY_PLUS(Query_for_list_of_roles,
								 "CURRENT_ROLE",
								 "CURRENT_USER",
								 "PUBLIC",
								 "USER");
	else if (Matches("ALTER|DROP", "USER", "MAPPING", "FOR"))
		COMPLETE_WITH_QUERY(Query_for_list_of_user_mappings);
	else if (Matches("CREATE|ALTER|DROP", "USER", "MAPPING", "FOR", MatchAny))
		COMPLETE_WITH("SERVER");
	else if (Matches("CREATE|ALTER", "USER", "MAPPING", "FOR", MatchAny, "SERVER", MatchAny))
		COMPLETE_WITH("OPTIONS");

/*
 * VACUUM [ ( option [, ...] ) ] [ table_and_columns [, ...] ]
 * VACUUM [ FULL ] [ FREEZE ] [ VERBOSE ] [ ANALYZE ] [ table_and_columns [, ...] ]
 */
	else if (Matches("VACUUM"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_vacuumables,
										"FULL",
										"FREEZE",
										"ANALYZE",
										"VERBOSE");
	else if (Matches("VACUUM", "FULL"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_vacuumables,
										"FREEZE",
										"ANALYZE",
										"VERBOSE");
	else if (Matches("VACUUM", "FREEZE") ||
			 Matches("VACUUM", "FULL", "FREEZE"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_vacuumables,
										"VERBOSE",
										"ANALYZE");
	else if (Matches("VACUUM", "VERBOSE") ||
			 Matches("VACUUM", "FULL|FREEZE", "VERBOSE") ||
			 Matches("VACUUM", "FULL", "FREEZE", "VERBOSE"))
		COMPLETE_WITH_SCHEMA_QUERY_PLUS(Query_for_list_of_vacuumables,
										"ANALYZE");
	else if (HeadMatches("VACUUM", "(*") &&
			 !HeadMatches("VACUUM", "(*)"))
	{
		/*
		 * This fires if we're in an unfinished parenthesized option list.
		 * get_previous_words treats a completed parenthesized option list as
		 * one word, so the above test is correct.
		 */
		if (ends_with(prev_wd, '(') || ends_with(prev_wd, ','))
			COMPLETE_WITH("FULL", "FREEZE", "ANALYZE", "VERBOSE",
						  "DISABLE_PAGE_SKIPPING", "SKIP_LOCKED",
						  "INDEX_CLEANUP", "PROCESS_MAIN", "PROCESS_TOAST",
						  "TRUNCATE", "PARALLEL", "SKIP_DATABASE_STATS",
						  "ONLY_DATABASE_STATS", "BUFFER_USAGE_LIMIT");
		else if (TailMatches("FULL|FREEZE|ANALYZE|VERBOSE|DISABLE_PAGE_SKIPPING|SKIP_LOCKED|PROCESS_MAIN|PROCESS_TOAST|TRUNCATE|SKIP_DATABASE_STATS|ONLY_DATABASE_STATS"))
			COMPLETE_WITH("ON", "OFF");
		else if (TailMatches("INDEX_CLEANUP"))
			COMPLETE_WITH("AUTO", "ON", "OFF");
	}
	else if (HeadMatches("VACUUM") && TailMatches("("))
		/* "VACUUM (" should be caught above, so assume we want columns */
		COMPLETE_WITH_ATTR(prev2_wd);
	else if (HeadMatches("VACUUM"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_vacuumables);

/* WITH [RECURSIVE] */

	/*
	 * Only match when WITH is the first word, as WITH may appear in many
	 * other contexts.
	 */
	else if (Matches("WITH"))
		COMPLETE_WITH("RECURSIVE");

/* WHERE */
	/* Simple case of the word before the where being the table name */
	else if (TailMatches(MatchAny, "WHERE"))
		COMPLETE_WITH_ATTR(prev2_wd);

/* ... FROM ... */
/* TODO: also include SRF ? */
	else if (TailMatches("FROM") && !Matches("COPY|\\copy", MatchAny, "FROM"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_selectables);

/* ... JOIN ... */
	else if (TailMatches("JOIN"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_selectables);

/* ... AT [ LOCAL | TIME ZONE ] ... */
	else if (TailMatches("AT"))
		COMPLETE_WITH("LOCAL", "TIME ZONE");
	else if (TailMatches("AT", "TIME", "ZONE"))
		COMPLETE_WITH_TIMEZONE_NAME();

/* Backslash commands */
/* TODO:  \dc \dd \dl */
	else if (TailMatchesCS("\\?"))
		COMPLETE_WITH_CS("commands", "options", "variables");
	else if (TailMatchesCS("\\connect|\\c"))
	{
		if (!recognized_connection_string(text))
			COMPLETE_WITH_QUERY(Query_for_list_of_databases);
	}
	else if (TailMatchesCS("\\connect|\\c", MatchAny))
	{
		if (!recognized_connection_string(prev_wd))
			COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	}
	else if (TailMatchesCS("\\da*"))
		COMPLETE_WITH_VERSIONED_SCHEMA_QUERY(Query_for_list_of_aggregates);
	else if (TailMatchesCS("\\dAc*", MatchAny) ||
			 TailMatchesCS("\\dAf*", MatchAny))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
	else if (TailMatchesCS("\\dAo*", MatchAny) ||
			 TailMatchesCS("\\dAp*", MatchAny))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_operator_families);
	else if (TailMatchesCS("\\dA*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_access_methods);
	else if (TailMatchesCS("\\db*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
	else if (TailMatchesCS("\\dconfig*"))
		COMPLETE_WITH_QUERY_VERBATIM(Query_for_list_of_show_vars);
	else if (TailMatchesCS("\\dD*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_domains);
	else if (TailMatchesCS("\\des*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_servers);
	else if (TailMatchesCS("\\deu*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_user_mappings);
	else if (TailMatchesCS("\\dew*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_fdws);
	else if (TailMatchesCS("\\df*"))
		COMPLETE_WITH_VERSIONED_SCHEMA_QUERY(Query_for_list_of_functions);
	else if (HeadMatchesCS("\\df*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);

	else if (TailMatchesCS("\\dFd*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_ts_dictionaries);
	else if (TailMatchesCS("\\dFp*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_ts_parsers);
	else if (TailMatchesCS("\\dFt*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_ts_templates);
	/* must be at end of \dF alternatives: */
	else if (TailMatchesCS("\\dF*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_ts_configurations);

	else if (TailMatchesCS("\\di*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes);
	else if (TailMatchesCS("\\dL*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_languages);
	else if (TailMatchesCS("\\dn*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
	/* no support for completing operators, but we can complete types: */
	else if (HeadMatchesCS("\\do*", MatchAny))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
	else if (TailMatchesCS("\\dp") || TailMatchesCS("\\z"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_grantables);
	else if (TailMatchesCS("\\dPi*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_partitioned_indexes);
	else if (TailMatchesCS("\\dPt*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_partitioned_tables);
	else if (TailMatchesCS("\\dP*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_partitioned_relations);
	else if (TailMatchesCS("\\dRp*"))
		COMPLETE_WITH_VERSIONED_QUERY(Query_for_list_of_publications);
	else if (TailMatchesCS("\\dRs*"))
		COMPLETE_WITH_VERSIONED_QUERY(Query_for_list_of_subscriptions);
	else if (TailMatchesCS("\\ds*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_sequences);
	else if (TailMatchesCS("\\dt*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	else if (TailMatchesCS("\\dT*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
	else if (TailMatchesCS("\\du*") ||
			 TailMatchesCS("\\dg*") ||
			 TailMatchesCS("\\drg*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (TailMatchesCS("\\dv*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views);
	else if (TailMatchesCS("\\dx*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_extensions);
	else if (TailMatchesCS("\\dX*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_statistics);
	else if (TailMatchesCS("\\dm*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews);
	else if (TailMatchesCS("\\dE*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_foreign_tables);
	else if (TailMatchesCS("\\dy*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);

	/* must be at end of \d alternatives: */
	else if (TailMatchesCS("\\d*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_relations);

	else if (TailMatchesCS("\\ef"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_routines);
	else if (TailMatchesCS("\\ev"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views);

	else if (TailMatchesCS("\\encoding"))
		COMPLETE_WITH_QUERY_VERBATIM(Query_for_list_of_encodings);
	else if (TailMatchesCS("\\h|\\help"))
		COMPLETE_WITH_LIST(sql_commands);
	else if (TailMatchesCS("\\h|\\help", MatchAny))
	{
		if (TailMatches("DROP"))
			matches = rl_completion_matches(text, drop_command_generator);
		else if (TailMatches("ALTER"))
			matches = rl_completion_matches(text, alter_command_generator);

		/*
		 * CREATE is recognized by tail match elsewhere, so doesn't need to be
		 * repeated here
		 */
	}
	else if (TailMatchesCS("\\h|\\help", MatchAny, MatchAny))
	{
		if (TailMatches("CREATE|DROP", "ACCESS"))
			COMPLETE_WITH("METHOD");
		else if (TailMatches("ALTER", "DEFAULT"))
			COMPLETE_WITH("PRIVILEGES");
		else if (TailMatches("CREATE|ALTER|DROP", "EVENT"))
			COMPLETE_WITH("TRIGGER");
		else if (TailMatches("CREATE|ALTER|DROP", "FOREIGN"))
			COMPLETE_WITH("DATA WRAPPER", "TABLE");
		else if (TailMatches("ALTER", "LARGE"))
			COMPLETE_WITH("OBJECT");
		else if (TailMatches("CREATE|ALTER|DROP", "MATERIALIZED"))
			COMPLETE_WITH("VIEW");
		else if (TailMatches("CREATE|ALTER|DROP", "TEXT"))
			COMPLETE_WITH("SEARCH");
		else if (TailMatches("CREATE|ALTER|DROP", "USER"))
			COMPLETE_WITH("MAPPING FOR");
	}
	else if (TailMatchesCS("\\h|\\help", MatchAny, MatchAny, MatchAny))
	{
		if (TailMatches("CREATE|ALTER|DROP", "FOREIGN", "DATA"))
			COMPLETE_WITH("WRAPPER");
		else if (TailMatches("CREATE|ALTER|DROP", "TEXT", "SEARCH"))
			COMPLETE_WITH("CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE");
		else if (TailMatches("CREATE|ALTER|DROP", "USER", "MAPPING"))
			COMPLETE_WITH("FOR");
	}
	else if (TailMatchesCS("\\l*") && !TailMatchesCS("\\lo*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_databases);
	else if (TailMatchesCS("\\password"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (TailMatchesCS("\\pset"))
		COMPLETE_WITH_CS("border", "columns", "csv_fieldsep", "expanded",
						 "fieldsep", "fieldsep_zero", "footer", "format",
						 "linestyle", "null", "numericlocale",
						 "pager", "pager_min_lines",
						 "recordsep", "recordsep_zero",
						 "tableattr", "title", "tuples_only",
						 "unicode_border_linestyle",
						 "unicode_column_linestyle",
						 "unicode_header_linestyle",
						 "xheader_width");
	else if (TailMatchesCS("\\pset", MatchAny))
	{
		if (TailMatchesCS("format"))
			COMPLETE_WITH_CS("aligned", "asciidoc", "csv", "html", "latex",
							 "latex-longtable", "troff-ms", "unaligned",
							 "wrapped");
		else if (TailMatchesCS("xheader_width"))
			COMPLETE_WITH_CS("full", "column", "page");
		else if (TailMatchesCS("linestyle"))
			COMPLETE_WITH_CS("ascii", "old-ascii", "unicode");
		else if (TailMatchesCS("pager"))
			COMPLETE_WITH_CS("on", "off", "always");
		else if (TailMatchesCS("unicode_border_linestyle|"
							   "unicode_column_linestyle|"
							   "unicode_header_linestyle"))
			COMPLETE_WITH_CS("single", "double");
	}
	else if (TailMatchesCS("\\unset"))
		matches = complete_from_variables(text, "", "", true);
	else if (TailMatchesCS("\\set"))
		matches = complete_from_variables(text, "", "", false);
	else if (TailMatchesCS("\\set", MatchAny))
	{
		if (TailMatchesCS("AUTOCOMMIT|ON_ERROR_STOP|QUIET|SHOW_ALL_RESULTS|"
						  "SINGLELINE|SINGLESTEP"))
			COMPLETE_WITH_CS("on", "off");
		else if (TailMatchesCS("COMP_KEYWORD_CASE"))
			COMPLETE_WITH_CS("lower", "upper",
							 "preserve-lower", "preserve-upper");
		else if (TailMatchesCS("ECHO"))
			COMPLETE_WITH_CS("errors", "queries", "all", "none");
		else if (TailMatchesCS("ECHO_HIDDEN"))
			COMPLETE_WITH_CS("noexec", "off", "on");
		else if (TailMatchesCS("HISTCONTROL"))
			COMPLETE_WITH_CS("ignorespace", "ignoredups",
							 "ignoreboth", "none");
		else if (TailMatchesCS("ON_ERROR_ROLLBACK"))
			COMPLETE_WITH_CS("on", "off", "interactive");
		else if (TailMatchesCS("SHOW_CONTEXT"))
			COMPLETE_WITH_CS("never", "errors", "always");
		else if (TailMatchesCS("VERBOSITY"))
			COMPLETE_WITH_CS("default", "verbose", "terse", "sqlstate");
	}
	else if (TailMatchesCS("\\sf*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_routines);
	else if (TailMatchesCS("\\sv*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views);
	else if (TailMatchesCS("\\cd|\\e|\\edit|\\g|\\gx|\\i|\\include|"
						   "\\ir|\\include_relative|\\o|\\out|"
						   "\\s|\\w|\\write|\\lo_import"))
	{
		completion_charp = "\\";
		completion_force_quote = false;
		matches = rl_completion_matches(text, complete_from_files);
	}

	/*
	 * Finally, we look through the list of "things", such as TABLE, INDEX and
	 * check if that was the previous word. If so, execute the query to get a
	 * list of them.
	 */
	else
	{
		const pgsql_thing_t *wac;

		for (wac = words_after_create; wac->name != NULL; wac++)
		{
			if (pg_strcasecmp(prev_wd, wac->name) == 0)
			{
				if (wac->query)
					COMPLETE_WITH_QUERY_LIST(wac->query,
											 wac->keywords);
				else if (wac->vquery)
					COMPLETE_WITH_VERSIONED_QUERY_LIST(wac->vquery,
													   wac->keywords);
				else if (wac->squery)
					COMPLETE_WITH_VERSIONED_SCHEMA_QUERY_LIST(wac->squery,
															  wac->keywords);
				break;
			}
		}
	}

	/*
	 * If we still don't have anything to match we have to fabricate some sort
	 * of default list. If we were to just return NULL, readline automatically
	 * attempts filename completion, and that's usually no good.
	 */
	if (matches == NULL)
	{
		COMPLETE_WITH_CONST(true, "");
		/* Also, prevent Readline from appending stuff to the non-match */
		rl_completion_append_character = '\0';
#ifdef HAVE_RL_COMPLETION_SUPPRESS_QUOTE
		rl_completion_suppress_quote = 1;
#endif
	}

	/* free storage */
	free(previous_words);
	free(words_buffer);
	free(text_copy);
	free(completion_ref_object);
	completion_ref_object = NULL;
	free(completion_ref_schema);
	completion_ref_schema = NULL;

	/* Return our Grand List O' Matches */
	return matches;
}


/*
 * GENERATOR FUNCTIONS
 *
 * These functions do all the actual work of completing the input. They get
 * passed the text so far and the count how many times they have been called
 * so far with the same text.
 * If you read the above carefully, you'll see that these don't get called
 * directly but through the readline interface.
 * The return value is expected to be the full completion of the text, going
 * through a list each time, or NULL if there are no more matches. The string
 * will be free()'d by readline, so you must run it through strdup() or
 * something of that sort.
 */

/*
 * Common routine for create_command_generator and drop_command_generator.
 * Entries that have 'excluded' flags are not returned.
 */
static char *
create_or_drop_command_generator(const char *text, int state, bits32 excluded)
{
	static int	list_index,
				string_length;
	const char *name;

	/* If this is the first time for this completion, init some values */
	if (state == 0)
	{
		list_index = 0;
		string_length = strlen(text);
	}

	/* find something that matches */
	while ((name = words_after_create[list_index++].name))
	{
		if ((pg_strncasecmp(name, text, string_length) == 0) &&
			!(words_after_create[list_index - 1].flags & excluded))
			return pg_strdup_keyword_case(name, text);
	}
	/* if nothing matches, return NULL */
	return NULL;
}

/*
 * This one gives you one from a list of things you can put after CREATE
 * as defined above.
 */
static char *
create_command_generator(const char *text, int state)
{
	return create_or_drop_command_generator(text, state, THING_NO_CREATE);
}

/*
 * This function gives you a list of things you can put after a DROP command.
 */
static char *
drop_command_generator(const char *text, int state)
{
	return create_or_drop_command_generator(text, state, THING_NO_DROP);
}

/*
 * This function gives you a list of things you can put after an ALTER command.
 */
static char *
alter_command_generator(const char *text, int state)
{
	return create_or_drop_command_generator(text, state, THING_NO_ALTER);
}

/*
 * These functions generate lists using server queries.
 * They are all wrappers for _complete_from_query.
 */

static char *
complete_from_query(const char *text, int state)
{
	/* query is assumed to work for any server version */
	return _complete_from_query(completion_charp, NULL, completion_charpp,
								completion_verbatim, text, state);
}

static char *
complete_from_versioned_query(const char *text, int state)
{
	const VersionedQuery *vquery = completion_vquery;

	/* Find appropriate array element */
	while (pset.sversion < vquery->min_server_version)
		vquery++;
	/* Fail completion if server is too old */
	if (vquery->query == NULL)
		return NULL;

	return _complete_from_query(vquery->query, NULL, completion_charpp,
								completion_verbatim, text, state);
}

static char *
complete_from_schema_query(const char *text, int state)
{
	/* query is assumed to work for any server version */
	return _complete_from_query(NULL, completion_squery, completion_charpp,
								completion_verbatim, text, state);
}

static char *
complete_from_versioned_schema_query(const char *text, int state)
{
	const SchemaQuery *squery = completion_squery;

	/* Find appropriate array element */
	while (pset.sversion < squery->min_server_version)
		squery++;
	/* Fail completion if server is too old */
	if (squery->catname == NULL)
		return NULL;

	return _complete_from_query(NULL, squery, completion_charpp,
								completion_verbatim, text, state);
}


/*
 * This creates a list of matching things, according to a query described by
 * the initial arguments.  The caller has already done any work needed to
 * select the appropriate query for the server's version.
 *
 * The query can be one of two kinds:
 *
 * 1. A simple query, which must contain a restriction clause of the form
 *		output LIKE '%s'
 * where "output" is the same string that the query returns.  The %s
 * will be replaced by a LIKE pattern to match the already-typed text.
 * There can be a second '%s', which will be replaced by a suitably-escaped
 * version of the string provided in completion_ref_object.  If there is a
 * third '%s', it will be replaced by a suitably-escaped version of the string
 * provided in completion_ref_schema.  Those strings should be set up
 * by calling set_completion_reference or set_completion_reference_verbatim.
 * Simple queries should return a single column of matches.  If "verbatim"
 * is true, the matches are returned as-is; otherwise, they are taken to
 * be SQL identifiers and quoted if necessary.
 *
 * 2. A schema query used for completion of both schema and relation names.
 * This is represented by a SchemaQuery object; see that typedef for details.
 *
 * See top of file for examples of both kinds of query.
 *
 * In addition to the query itself, we accept a null-terminated array of
 * literal keywords, which will be returned if they match the input-so-far
 * (case insensitively).  (These are in addition to keywords specified
 * within the schema_query, if any.)
 *
 * If "verbatim" is true, then we use the given text as-is to match the
 * query results; otherwise we parse it as a possibly-qualified identifier,
 * and reconstruct suitable quoting afterward.
 *
 * "text" and "state" are supplied by Readline.  "text" is the word we are
 * trying to complete.  "state" is zero on first call, nonzero later.
 *
 * readline will call this repeatedly with the same text and varying
 * state.  On each call, we are supposed to return a malloc'd string
 * that is a candidate completion.  Return NULL when done.
 */
static char *
_complete_from_query(const char *simple_query,
					 const SchemaQuery *schema_query,
					 const char *const *keywords,
					 bool verbatim,
					 const char *text, int state)
{
	static int	list_index,
				num_schema_only,
				num_query_other,
				num_keywords;
	static PGresult *result = NULL;
	static bool non_empty_object;
	static bool schemaquoted;
	static bool objectquoted;

	/*
	 * If this is the first time for this completion, we fetch a list of our
	 * "things" from the backend.
	 */
	if (state == 0)
	{
		PQExpBufferData query_buffer;
		char	   *schemaname;
		char	   *objectname;
		char	   *e_object_like;
		char	   *e_schemaname;
		char	   *e_ref_object;
		char	   *e_ref_schema;

		/* Reset static state, ensuring no memory leaks */
		list_index = 0;
		num_schema_only = 0;
		num_query_other = 0;
		num_keywords = 0;
		PQclear(result);
		result = NULL;

		/* Parse text, splitting into schema and object name if needed */
		if (verbatim)
		{
			objectname = pg_strdup(text);
			schemaname = NULL;
		}
		else
		{
			parse_identifier(text,
							 &schemaname, &objectname,
							 &schemaquoted, &objectquoted);
		}

		/* Remember whether the user has typed anything in the object part */
		non_empty_object = (*objectname != '\0');

		/*
		 * Convert objectname to a LIKE prefix pattern (e.g. 'foo%'), and set
		 * up suitably-escaped copies of all the strings we need.
		 */
		e_object_like = make_like_pattern(objectname);

		if (schemaname)
			e_schemaname = escape_string(schemaname);
		else
			e_schemaname = NULL;

		if (completion_ref_object)
			e_ref_object = escape_string(completion_ref_object);
		else
			e_ref_object = NULL;

		if (completion_ref_schema)
			e_ref_schema = escape_string(completion_ref_schema);
		else
			e_ref_schema = NULL;

		initPQExpBuffer(&query_buffer);

		if (schema_query)
		{
			Assert(simple_query == NULL);

			/*
			 * We issue different queries depending on whether the input is
			 * already qualified or not.  schema_query gives us the pieces to
			 * assemble.
			 */
			if (schemaname == NULL || schema_query->namespace == NULL)
			{
				/* Get unqualified names matching the input-so-far */
				appendPQExpBufferStr(&query_buffer, "SELECT ");
				if (schema_query->use_distinct)
					appendPQExpBufferStr(&query_buffer, "DISTINCT ");
				appendPQExpBuffer(&query_buffer,
								  "%s, NULL::pg_catalog.text FROM %s",
								  schema_query->result,
								  schema_query->catname);
				if (schema_query->refnamespace && completion_ref_schema)
					appendPQExpBufferStr(&query_buffer,
										 ", pg_catalog.pg_namespace nr");
				appendPQExpBufferStr(&query_buffer, " WHERE ");
				if (schema_query->selcondition)
					appendPQExpBuffer(&query_buffer, "%s AND ",
									  schema_query->selcondition);
				appendPQExpBuffer(&query_buffer, "(%s) LIKE '%s'",
								  schema_query->result,
								  e_object_like);
				if (schema_query->viscondition)
					appendPQExpBuffer(&query_buffer, " AND %s",
									  schema_query->viscondition);
				if (schema_query->refname)
				{
					Assert(completion_ref_object);
					appendPQExpBuffer(&query_buffer, " AND %s = '%s'",
									  schema_query->refname, e_ref_object);
					if (schema_query->refnamespace && completion_ref_schema)
						appendPQExpBuffer(&query_buffer,
										  " AND %s = nr.oid AND nr.nspname = '%s'",
										  schema_query->refnamespace,
										  e_ref_schema);
					else if (schema_query->refviscondition)
						appendPQExpBuffer(&query_buffer,
										  " AND %s",
										  schema_query->refviscondition);
				}

				/*
				 * When fetching relation names, suppress system catalogs
				 * unless the input-so-far begins with "pg_".  This is a
				 * compromise between not offering system catalogs for
				 * completion at all, and having them swamp the result when
				 * the input is just "p".
				 */
				if (strcmp(schema_query->catname,
						   "pg_catalog.pg_class c") == 0 &&
					strncmp(objectname, "pg_", 3) != 0)
				{
					appendPQExpBufferStr(&query_buffer,
										 " AND c.relnamespace <> (SELECT oid FROM"
										 " pg_catalog.pg_namespace WHERE nspname = 'pg_catalog')");
				}

				/*
				 * If the target object type can be schema-qualified, add in
				 * schema names matching the input-so-far.
				 */
				if (schema_query->namespace)
				{
					appendPQExpBuffer(&query_buffer, "\nUNION ALL\n"
									  "SELECT NULL::pg_catalog.text, n.nspname "
									  "FROM pg_catalog.pg_namespace n "
									  "WHERE n.nspname LIKE '%s'",
									  e_object_like);

					/*
					 * Likewise, suppress system schemas unless the
					 * input-so-far begins with "pg_".
					 */
					if (strncmp(objectname, "pg_", 3) != 0)
						appendPQExpBufferStr(&query_buffer,
											 " AND n.nspname NOT LIKE E'pg\\\\_%'");

					/*
					 * Since we're matching these schema names to the object
					 * name, handle their quoting using the object name's
					 * quoting state.
					 */
					schemaquoted = objectquoted;
				}
			}
			else
			{
				/* Input is qualified, so produce only qualified names */
				appendPQExpBufferStr(&query_buffer, "SELECT ");
				if (schema_query->use_distinct)
					appendPQExpBufferStr(&query_buffer, "DISTINCT ");
				appendPQExpBuffer(&query_buffer, "%s, n.nspname "
								  "FROM %s, pg_catalog.pg_namespace n",
								  schema_query->result,
								  schema_query->catname);
				if (schema_query->refnamespace && completion_ref_schema)
					appendPQExpBufferStr(&query_buffer,
										 ", pg_catalog.pg_namespace nr");
				appendPQExpBuffer(&query_buffer, " WHERE %s = n.oid AND ",
								  schema_query->namespace);
				if (schema_query->selcondition)
					appendPQExpBuffer(&query_buffer, "%s AND ",
									  schema_query->selcondition);
				appendPQExpBuffer(&query_buffer, "(%s) LIKE '%s' AND ",
								  schema_query->result,
								  e_object_like);
				appendPQExpBuffer(&query_buffer, "n.nspname = '%s'",
								  e_schemaname);
				if (schema_query->refname)
				{
					Assert(completion_ref_object);
					appendPQExpBuffer(&query_buffer, " AND %s = '%s'",
									  schema_query->refname, e_ref_object);
					if (schema_query->refnamespace && completion_ref_schema)
						appendPQExpBuffer(&query_buffer,
										  " AND %s = nr.oid AND nr.nspname = '%s'",
										  schema_query->refnamespace,
										  e_ref_schema);
					else if (schema_query->refviscondition)
						appendPQExpBuffer(&query_buffer,
										  " AND %s",
										  schema_query->refviscondition);
				}
			}
		}
		else
		{
			Assert(simple_query);
			/* simple_query is an sprintf-style format string */
			appendPQExpBuffer(&query_buffer, simple_query,
							  e_object_like,
							  e_ref_object, e_ref_schema);
		}

		/* Limit the number of records in the result */
		appendPQExpBuffer(&query_buffer, "\nLIMIT %d",
						  completion_max_records);

		/* Finally, we can issue the query */
		result = exec_query(query_buffer.data);

		/* Clean up */
		termPQExpBuffer(&query_buffer);
		free(schemaname);
		free(objectname);
		free(e_object_like);
		free(e_schemaname);
		free(e_ref_object);
		free(e_ref_schema);
	}

	/* Return the next result, if any, but not if the query failed */
	if (result && PQresultStatus(result) == PGRES_TUPLES_OK)
	{
		int			nskip;

		while (list_index < PQntuples(result))
		{
			const char *item = NULL;
			const char *nsp = NULL;

			if (!PQgetisnull(result, list_index, 0))
				item = PQgetvalue(result, list_index, 0);
			if (PQnfields(result) > 1 &&
				!PQgetisnull(result, list_index, 1))
				nsp = PQgetvalue(result, list_index, 1);
			list_index++;

			/* In verbatim mode, we return all the items as-is */
			if (verbatim)
			{
				num_query_other++;
				return pg_strdup(item);
			}

			/*
			 * In normal mode, a name requiring quoting will be returned only
			 * if the input was empty or quoted.  Otherwise the user might see
			 * completion inserting a quote she didn't type, which is
			 * surprising.  This restriction also dodges some odd behaviors of
			 * some versions of readline/libedit.
			 */
			if (non_empty_object)
			{
				if (item && !objectquoted && identifier_needs_quotes(item))
					continue;
				if (nsp && !schemaquoted && identifier_needs_quotes(nsp))
					continue;
			}

			/* Count schema-only results for hack below */
			if (item == NULL && nsp != NULL)
				num_schema_only++;
			else
				num_query_other++;

			return requote_identifier(nsp, item, schemaquoted, objectquoted);
		}

		/*
		 * When the query result is exhausted, check for hard-wired keywords.
		 * These will only be returned if they match the input-so-far,
		 * ignoring case.
		 */
		nskip = list_index - PQntuples(result);
		if (schema_query && schema_query->keywords)
		{
			const char *const *itemp = schema_query->keywords;

			while (*itemp)
			{
				const char *item = *itemp++;

				if (nskip-- > 0)
					continue;
				list_index++;
				if (pg_strncasecmp(text, item, strlen(text)) == 0)
				{
					num_keywords++;
					return pg_strdup_keyword_case(item, text);
				}
			}
		}
		if (keywords)
		{
			const char *const *itemp = keywords;

			while (*itemp)
			{
				const char *item = *itemp++;

				if (nskip-- > 0)
					continue;
				list_index++;
				if (pg_strncasecmp(text, item, strlen(text)) == 0)
				{
					num_keywords++;
					return pg_strdup_keyword_case(item, text);
				}
			}
		}
	}

	/*
	 * Hack: if we returned only bare schema names, don't let Readline add a
	 * space afterwards.  Otherwise the schema will stop being part of the
	 * completion subject text, which is not what we want.
	 */
	if (num_schema_only > 0 && num_query_other == 0 && num_keywords == 0)
		rl_completion_append_character = '\0';

	/* No more matches, so free the result structure and return null */
	PQclear(result);
	result = NULL;
	return NULL;
}


/*
 * Set up completion_ref_object and completion_ref_schema
 * by parsing the given word.  These variables can then be
 * used in a query passed to _complete_from_query.
 */
static void
set_completion_reference(const char *word)
{
	bool		schemaquoted,
				objectquoted;

	parse_identifier(word,
					 &completion_ref_schema, &completion_ref_object,
					 &schemaquoted, &objectquoted);
}

/*
 * Set up completion_ref_object when it should just be
 * the given word verbatim.
 */
static void
set_completion_reference_verbatim(const char *word)
{
	completion_ref_schema = NULL;
	completion_ref_object = pg_strdup(word);
}


/*
 * This function returns in order one of a fixed, NULL pointer terminated list
 * of strings (if matching). This can be used if there are only a fixed number
 * SQL words that can appear at certain spot.
 */
static char *
complete_from_list(const char *text, int state)
{
	static int	string_length,
				list_index,
				matches;
	static bool casesensitive;
	const char *item;

	/* need to have a list */
	Assert(completion_charpp != NULL);

	/* Initialization */
	if (state == 0)
	{
		list_index = 0;
		string_length = strlen(text);
		casesensitive = completion_case_sensitive;
		matches = 0;
	}

	while ((item = completion_charpp[list_index++]))
	{
		/* First pass is case sensitive */
		if (casesensitive && strncmp(text, item, string_length) == 0)
		{
			matches++;
			return pg_strdup(item);
		}

		/* Second pass is case insensitive, don't bother counting matches */
		if (!casesensitive && pg_strncasecmp(text, item, string_length) == 0)
		{
			if (completion_case_sensitive)
				return pg_strdup(item);
			else

				/*
				 * If case insensitive matching was requested initially,
				 * adjust the case according to setting.
				 */
				return pg_strdup_keyword_case(item, text);
		}
	}

	/*
	 * No matches found. If we're not case insensitive already, lets switch to
	 * being case insensitive and try again
	 */
	if (casesensitive && matches == 0)
	{
		casesensitive = false;
		list_index = 0;
		state++;
		return complete_from_list(text, state);
	}

	/* If no more matches, return null. */
	return NULL;
}


/*
 * This function returns one fixed string the first time even if it doesn't
 * match what's there, and nothing the second time.  The string
 * to be used must be in completion_charp.
 *
 * If the given string is "", this has the effect of preventing readline
 * from doing any completion.  (Without this, readline tries to do filename
 * completion which is seldom the right thing.)
 *
 * If the given string is not empty, readline will replace whatever the
 * user typed with that string.  This behavior might be useful if it's
 * completely certain that we know what must appear at a certain spot,
 * so that it's okay to overwrite misspellings.  In practice, given the
 * relatively lame parsing technology used in this file, the level of
 * certainty is seldom that high, so that you probably don't want to
 * use this.  Use complete_from_list with a one-element list instead;
 * that won't try to auto-correct "misspellings".
 */
static char *
complete_from_const(const char *text, int state)
{
	Assert(completion_charp != NULL);
	if (state == 0)
	{
		if (completion_case_sensitive)
			return pg_strdup(completion_charp);
		else

			/*
			 * If case insensitive matching was requested initially, adjust
			 * the case according to setting.
			 */
			return pg_strdup_keyword_case(completion_charp, text);
	}
	else
		return NULL;
}


/*
 * This function appends the variable name with prefix and suffix to
 * the variable names array.
 */
static void
append_variable_names(char ***varnames, int *nvars,
					  int *maxvars, const char *varname,
					  const char *prefix, const char *suffix)
{
	if (*nvars >= *maxvars)
	{
		*maxvars *= 2;
		*varnames = (char **) pg_realloc(*varnames,
										 ((*maxvars) + 1) * sizeof(char *));
	}

	(*varnames)[(*nvars)++] = psprintf("%s%s%s", prefix, varname, suffix);
}


/*
 * This function supports completion with the name of a psql variable.
 * The variable names can be prefixed and suffixed with additional text
 * to support quoting usages. If need_value is true, only variables
 * that are currently set are included; otherwise, special variables
 * (those that have hooks) are included even if currently unset.
 */
static char **
complete_from_variables(const char *text, const char *prefix, const char *suffix,
						bool need_value)
{
	char	  **matches;
	char	  **varnames;
	int			nvars = 0;
	int			maxvars = 100;
	int			i;
	struct _variable *ptr;

	varnames = (char **) pg_malloc((maxvars + 1) * sizeof(char *));

	for (ptr = pset.vars->next; ptr; ptr = ptr->next)
	{
		if (need_value && !(ptr->value))
			continue;
		append_variable_names(&varnames, &nvars, &maxvars, ptr->name,
							  prefix, suffix);
	}

	varnames[nvars] = NULL;
	COMPLETE_WITH_LIST_CS((const char *const *) varnames);

	for (i = 0; i < nvars; i++)
		free(varnames[i]);
	free(varnames);

	return matches;
}


/*
 * This function wraps rl_filename_completion_function() to strip quotes from
 * the input before searching for matches and to quote any matches for which
 * the consuming command will require it.
 *
 * Caller must set completion_charp to a zero- or one-character string
 * containing the escape character.  This is necessary since \copy has no
 * escape character, but every other backslash command recognizes "\" as an
 * escape character.
 *
 * Caller must also set completion_force_quote to indicate whether to force
 * quotes around the result.  (The SQL COPY command requires that.)
 */
static char *
complete_from_files(const char *text, int state)
{
#ifdef USE_FILENAME_QUOTING_FUNCTIONS

	/*
	 * If we're using a version of Readline that supports filename quoting
	 * hooks, rely on those, and invoke rl_filename_completion_function()
	 * without messing with its arguments.  Readline does stuff internally
	 * that does not work well at all if we try to handle dequoting here.
	 * Instead, Readline will call quote_file_name() and dequote_file_name()
	 * (see below) at appropriate times.
	 *
	 * ... or at least, mostly it will.  There are some paths involving
	 * unmatched file names in which Readline never calls quote_file_name(),
	 * and if left to its own devices it will incorrectly append a quote
	 * anyway.  Set rl_completion_suppress_quote to prevent that.  If we do
	 * get to quote_file_name(), we'll clear this again.  (Yes, this seems
	 * like it's working around Readline bugs.)
	 */
#ifdef HAVE_RL_COMPLETION_SUPPRESS_QUOTE
	rl_completion_suppress_quote = 1;
#endif

	/* If user typed a quote, force quoting (never remove user's quote) */
	if (*text == '\'')
		completion_force_quote = true;

	return rl_filename_completion_function(text, state);
#else

	/*
	 * Otherwise, we have to do the best we can.
	 */
	static const char *unquoted_text;
	char	   *unquoted_match;
	char	   *ret = NULL;

	/* If user typed a quote, force quoting (never remove user's quote) */
	if (*text == '\'')
		completion_force_quote = true;

	if (state == 0)
	{
		/* Initialization: stash the unquoted input. */
		unquoted_text = strtokx(text, "", NULL, "'", *completion_charp,
								false, true, pset.encoding);
		/* expect a NULL return for the empty string only */
		if (!unquoted_text)
		{
			Assert(*text == '\0');
			unquoted_text = text;
		}
	}

	unquoted_match = rl_filename_completion_function(unquoted_text, state);
	if (unquoted_match)
	{
		struct stat statbuf;
		bool		is_dir = (stat(unquoted_match, &statbuf) == 0 &&
							  S_ISDIR(statbuf.st_mode) != 0);

		/* Re-quote the result, if needed. */
		ret = quote_if_needed(unquoted_match, " \t\r\n\"`",
							  '\'', *completion_charp,
							  completion_force_quote,
							  pset.encoding);
		if (ret)
			free(unquoted_match);
		else
			ret = unquoted_match;

		/*
		 * If it's a directory, replace trailing quote with a slash; this is
		 * usually more convenient.  (If we didn't quote, leave this to
		 * libedit.)
		 */
		if (*ret == '\'' && is_dir)
		{
			char	   *retend = ret + strlen(ret) - 1;

			Assert(*retend == '\'');
			*retend = '/';
			/* Prevent libedit from adding a space, too */
			rl_completion_append_character = '\0';
		}
	}

	return ret;
#endif							/* USE_FILENAME_QUOTING_FUNCTIONS */
}


/* HELPER FUNCTIONS */


/*
 * Make a pg_strdup copy of s and convert the case according to
 * COMP_KEYWORD_CASE setting, using ref as the text that was already entered.
 */
static char *
pg_strdup_keyword_case(const char *s, const char *ref)
{
	char	   *ret,
			   *p;
	unsigned char first = ref[0];

	ret = pg_strdup(s);

	if (pset.comp_case == PSQL_COMP_CASE_LOWER ||
		((pset.comp_case == PSQL_COMP_CASE_PRESERVE_LOWER ||
		  pset.comp_case == PSQL_COMP_CASE_PRESERVE_UPPER) && islower(first)) ||
		(pset.comp_case == PSQL_COMP_CASE_PRESERVE_LOWER && !isalpha(first)))
	{
		for (p = ret; *p; p++)
			*p = pg_tolower((unsigned char) *p);
	}
	else
	{
		for (p = ret; *p; p++)
			*p = pg_toupper((unsigned char) *p);
	}

	return ret;
}


/*
 * escape_string - Escape argument for use as string literal.
 *
 * The returned value has to be freed.
 */
static char *
escape_string(const char *text)
{
	size_t		text_length;
	char	   *result;

	text_length = strlen(text);

	result = pg_malloc(text_length * 2 + 1);
	PQescapeStringConn(pset.db, result, text, text_length, NULL);

	return result;
}


/*
 * make_like_pattern - Convert argument to a LIKE prefix pattern.
 *
 * We escape _ and % in the given text by backslashing, append a % to
 * represent "any subsequent characters", and then pass the string through
 * escape_string() so it's ready to insert in a query.  The result needs
 * to be freed.
 */
static char *
make_like_pattern(const char *word)
{
	char	   *result;
	char	   *buffer = pg_malloc(strlen(word) * 2 + 2);
	char	   *bptr = buffer;

	while (*word)
	{
		if (*word == '_' || *word == '%')
			*bptr++ = '\\';
		if (IS_HIGHBIT_SET(*word))
		{
			/*
			 * Transfer multibyte characters without further processing, to
			 * avoid getting confused in unsafe client encodings.
			 */
			int			chlen = PQmblenBounded(word, pset.encoding);

			while (chlen-- > 0)
				*bptr++ = *word++;
		}
		else
			*bptr++ = *word++;
	}
	*bptr++ = '%';
	*bptr = '\0';

	result = escape_string(buffer);
	free(buffer);
	return result;
}


/*
 * parse_identifier - Parse a possibly-schema-qualified SQL identifier.
 *
 * This involves splitting off the schema name if present, de-quoting,
 * and downcasing any unquoted text.  We are a bit laxer than the backend
 * in that we allow just portions of a name to be quoted --- that's because
 * psql metacommands have traditionally behaved that way.
 *
 * Outputs are a malloc'd schema name (NULL if none), malloc'd object name,
 * and booleans telling whether any part of the schema and object name was
 * double-quoted.
 */
static void
parse_identifier(const char *ident,
				 char **schemaname, char **objectname,
				 bool *schemaquoted, bool *objectquoted)
{
	size_t		buflen = strlen(ident) + 1;
	bool		enc_is_single_byte = (pg_encoding_max_length(pset.encoding) == 1);
	char	   *sname;
	char	   *oname;
	char	   *optr;
	bool		inquotes;

	/* Initialize, making a certainly-large-enough output buffer */
	sname = NULL;
	oname = pg_malloc(buflen);
	*schemaquoted = *objectquoted = false;
	/* Scan */
	optr = oname;
	inquotes = false;
	while (*ident)
	{
		unsigned char ch = (unsigned char) *ident++;

		if (ch == '"')
		{
			if (inquotes && *ident == '"')
			{
				/* two quote marks within a quoted identifier = emit quote */
				*optr++ = '"';
				ident++;
			}
			else
			{
				inquotes = !inquotes;
				*objectquoted = true;
			}
		}
		else if (ch == '.' && !inquotes)
		{
			/* Found a schema name, transfer it to sname / *schemaquoted */
			*optr = '\0';
			free(sname);		/* drop any catalog name */
			sname = oname;
			oname = pg_malloc(buflen);
			optr = oname;
			*schemaquoted = *objectquoted;
			*objectquoted = false;
		}
		else if (!enc_is_single_byte && IS_HIGHBIT_SET(ch))
		{
			/*
			 * Transfer multibyte characters without further processing.  They
			 * wouldn't be affected by our downcasing rule anyway, and this
			 * avoids possibly doing the wrong thing in unsafe client
			 * encodings.
			 */
			int			chlen = PQmblenBounded(ident - 1, pset.encoding);

			*optr++ = (char) ch;
			while (--chlen > 0)
				*optr++ = *ident++;
		}
		else
		{
			if (!inquotes)
			{
				/*
				 * This downcasing transformation should match the backend's
				 * downcase_identifier() as best we can.  We do not know the
				 * backend's locale, though, so it's necessarily approximate.
				 * We assume that psql is operating in the same locale and
				 * encoding as the backend.
				 */
				if (ch >= 'A' && ch <= 'Z')
					ch += 'a' - 'A';
				else if (enc_is_single_byte && IS_HIGHBIT_SET(ch) && isupper(ch))
					ch = tolower(ch);
			}
			*optr++ = (char) ch;
		}
	}

	*optr = '\0';
	*schemaname = sname;
	*objectname = oname;
}


/*
 * requote_identifier - Reconstruct a possibly-schema-qualified SQL identifier.
 *
 * Build a malloc'd string containing the identifier, with quoting applied
 * as necessary.  This is more or less the inverse of parse_identifier;
 * in particular, if an input component was quoted, we'll quote the output
 * even when that isn't strictly required.
 *
 * Unlike parse_identifier, we handle the case where a schema and no
 * object name is provided, producing just "schema.".
 */
static char *
requote_identifier(const char *schemaname, const char *objectname,
				   bool quote_schema, bool quote_object)
{
	char	   *result;
	size_t		buflen = 1;		/* count the trailing \0 */
	char	   *ptr;

	/*
	 * We could use PQescapeIdentifier for some of this, but not all, and it
	 * adds more notational cruft than it seems worth.
	 */
	if (schemaname)
	{
		buflen += strlen(schemaname) + 1;	/* +1 for the dot */
		if (!quote_schema)
			quote_schema = identifier_needs_quotes(schemaname);
		if (quote_schema)
		{
			buflen += 2;		/* account for quote marks */
			for (const char *p = schemaname; *p; p++)
			{
				if (*p == '"')
					buflen++;
			}
		}
	}
	if (objectname)
	{
		buflen += strlen(objectname);
		if (!quote_object)
			quote_object = identifier_needs_quotes(objectname);
		if (quote_object)
		{
			buflen += 2;		/* account for quote marks */
			for (const char *p = objectname; *p; p++)
			{
				if (*p == '"')
					buflen++;
			}
		}
	}
	result = pg_malloc(buflen);
	ptr = result;
	if (schemaname)
	{
		if (quote_schema)
			*ptr++ = '"';
		for (const char *p = schemaname; *p; p++)
		{
			*ptr++ = *p;
			if (*p == '"')
				*ptr++ = '"';
		}
		if (quote_schema)
			*ptr++ = '"';
		*ptr++ = '.';
	}
	if (objectname)
	{
		if (quote_object)
			*ptr++ = '"';
		for (const char *p = objectname; *p; p++)
		{
			*ptr++ = *p;
			if (*p == '"')
				*ptr++ = '"';
		}
		if (quote_object)
			*ptr++ = '"';
	}
	*ptr = '\0';
	return result;
}


/*
 * Detect whether an identifier must be double-quoted.
 *
 * Note we'll quote anything that's not ASCII; the backend's quote_ident()
 * does the same.  Perhaps this could be relaxed in future.
 */
static bool
identifier_needs_quotes(const char *ident)
{
	int			kwnum;

	/* Check syntax. */
	if (!((ident[0] >= 'a' && ident[0] <= 'z') || ident[0] == '_'))
		return true;
	if (strspn(ident, "abcdefghijklmnopqrstuvwxyz0123456789_$") != strlen(ident))
		return true;

	/*
	 * Check for keyword.  We quote keywords except for unreserved ones.
	 *
	 * It is possible that our keyword list doesn't quite agree with the
	 * server's, but this should be close enough for tab-completion purposes.
	 *
	 * Note: ScanKeywordLookup() does case-insensitive comparison, but that's
	 * fine, since we already know we have all-lower-case.
	 */
	kwnum = ScanKeywordLookup(ident, &ScanKeywords);

	if (kwnum >= 0 && ScanKeywordCategories[kwnum] != UNRESERVED_KEYWORD)
		return true;

	return false;
}


/*
 * Execute a query, returning NULL if there was any error.
 * This should be the preferred way of talking to the database in this file.
 */
static PGresult *
exec_query(const char *query)
{
	PGresult   *result;

	if (query == NULL || !pset.db || PQstatus(pset.db) != CONNECTION_OK)
		return NULL;

	result = PQexec(pset.db, query);

	if (PQresultStatus(result) != PGRES_TUPLES_OK)
	{
		/*
		 * Printing an error while the user is typing would be quite annoying,
		 * so we don't.  This does complicate debugging of this code; but you
		 * can look in the server log instead.
		 */
#ifdef NOT_USED
		pg_log_error("tab completion query failed: %s\nQuery was:\n%s",
					 PQerrorMessage(pset.db), query);
#endif
		PQclear(result);
		result = NULL;
	}

	return result;
}


/*
 * Parse all the word(s) before point.
 *
 * Returns a malloc'd array of character pointers that point into the malloc'd
 * data array returned to *buffer; caller must free() both of these when done.
 * *nwords receives the number of words found, ie, the valid length of the
 * return array.
 *
 * Words are returned right to left, that is, previous_words[0] gets the last
 * word before point, previous_words[1] the next-to-last, etc.
 */
static char **
get_previous_words(int point, char **buffer, int *nwords)
{
	char	  **previous_words;
	char	   *buf;
	char	   *outptr;
	int			words_found = 0;
	int			i;

	/*
	 * If we have anything in tab_completion_query_buf, paste it together with
	 * rl_line_buffer to construct the full query.  Otherwise we can just use
	 * rl_line_buffer as the input string.
	 */
	if (tab_completion_query_buf && tab_completion_query_buf->len > 0)
	{
		i = tab_completion_query_buf->len;
		buf = pg_malloc(point + i + 2);
		memcpy(buf, tab_completion_query_buf->data, i);
		buf[i++] = '\n';
		memcpy(buf + i, rl_line_buffer, point);
		i += point;
		buf[i] = '\0';
		/* Readjust point to reference appropriate offset in buf */
		point = i;
	}
	else
		buf = rl_line_buffer;

	/*
	 * Allocate an array of string pointers and a buffer to hold the strings
	 * themselves.  The worst case is that the line contains only
	 * non-whitespace WORD_BREAKS characters, making each one a separate word.
	 * This is usually much more space than we need, but it's cheaper than
	 * doing a separate malloc() for each word.
	 */
	previous_words = (char **) pg_malloc(point * sizeof(char *));
	*buffer = outptr = (char *) pg_malloc(point * 2);

	/*
	 * First we look for a non-word char before the current point.  (This is
	 * probably useless, if readline is on the same page as we are about what
	 * is a word, but if so it's cheap.)
	 */
	for (i = point - 1; i >= 0; i--)
	{
		if (strchr(WORD_BREAKS, buf[i]))
			break;
	}
	point = i;

	/*
	 * Now parse words, working backwards, until we hit start of line.  The
	 * backwards scan has some interesting but intentional properties
	 * concerning parenthesis handling.
	 */
	while (point >= 0)
	{
		int			start,
					end;
		bool		inquotes = false;
		int			parentheses = 0;

		/* now find the first non-space which then constitutes the end */
		end = -1;
		for (i = point; i >= 0; i--)
		{
			if (!isspace((unsigned char) buf[i]))
			{
				end = i;
				break;
			}
		}
		/* if no end found, we're done */
		if (end < 0)
			break;

		/*
		 * Otherwise we now look for the start.  The start is either the last
		 * character before any word-break character going backwards from the
		 * end, or it's simply character 0.  We also handle open quotes and
		 * parentheses.
		 */
		for (start = end; start > 0; start--)
		{
			if (buf[start] == '"')
				inquotes = !inquotes;
			if (!inquotes)
			{
				if (buf[start] == ')')
					parentheses++;
				else if (buf[start] == '(')
				{
					if (--parentheses <= 0)
						break;
				}
				else if (parentheses == 0 &&
						 strchr(WORD_BREAKS, buf[start - 1]))
					break;
			}
		}

		/* Return the word located at start to end inclusive */
		previous_words[words_found++] = outptr;
		i = end - start + 1;
		memcpy(outptr, &buf[start], i);
		outptr += i;
		*outptr++ = '\0';

		/* Continue searching */
		point = start - 1;
	}

	/* Release parsing input workspace, if we made one above */
	if (buf != rl_line_buffer)
		free(buf);

	*nwords = words_found;
	return previous_words;
}

/*
 * Look up the type for the GUC variable with the passed name.
 *
 * Returns NULL if the variable is unknown. Otherwise the returned string,
 * containing the type, has to be freed.
 */
static char *
get_guctype(const char *varname)
{
	PQExpBufferData query_buffer;
	char	   *e_varname;
	PGresult   *result;
	char	   *guctype = NULL;

	e_varname = escape_string(varname);

	initPQExpBuffer(&query_buffer);
	appendPQExpBuffer(&query_buffer,
					  "SELECT vartype FROM pg_catalog.pg_settings "
					  "WHERE pg_catalog.lower(name) = pg_catalog.lower('%s')",
					  e_varname);

	result = exec_query(query_buffer.data);
	termPQExpBuffer(&query_buffer);
	free(e_varname);

	if (PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0)
		guctype = pg_strdup(PQgetvalue(result, 0, 0));

	PQclear(result);

	return guctype;
}

#ifdef USE_FILENAME_QUOTING_FUNCTIONS

/*
 * Quote a filename according to SQL rules, returning a malloc'd string.
 * completion_charp must point to escape character or '\0', and
 * completion_force_quote must be set correctly, as per comments for
 * complete_from_files().
 */
static char *
quote_file_name(char *fname, int match_type, char *quote_pointer)
{
	char	   *s;
	struct stat statbuf;

	/* Quote if needed. */
	s = quote_if_needed(fname, " \t\r\n\"`",
						'\'', *completion_charp,
						completion_force_quote,
						pset.encoding);
	if (!s)
		s = pg_strdup(fname);

	/*
	 * However, some of the time we have to strip the trailing quote from what
	 * we send back.  Never strip the trailing quote if the user already typed
	 * one; otherwise, suppress the trailing quote if we have multiple/no
	 * matches (because we don't want to add a quote if the input is seemingly
	 * unfinished), or if the input was already quoted (because Readline will
	 * do arguably-buggy things otherwise), or if the file does not exist, or
	 * if it's a directory.
	 */
	if (*s == '\'' &&
		completion_last_char != '\'' &&
		(match_type != SINGLE_MATCH ||
		 (quote_pointer && *quote_pointer == '\'') ||
		 stat(fname, &statbuf) != 0 ||
		 S_ISDIR(statbuf.st_mode)))
	{
		char	   *send = s + strlen(s) - 1;

		Assert(*send == '\'');
		*send = '\0';
	}

	/*
	 * And now we can let Readline do its thing with possibly adding a quote
	 * on its own accord.  (This covers some additional cases beyond those
	 * dealt with above.)
	 */
#ifdef HAVE_RL_COMPLETION_SUPPRESS_QUOTE
	rl_completion_suppress_quote = 0;
#endif

	/*
	 * If user typed a leading quote character other than single quote (i.e.,
	 * double quote), zap it, so that we replace it with the correct single
	 * quote.
	 */
	if (quote_pointer && *quote_pointer != '\'')
		*quote_pointer = '\0';

	return s;
}

/*
 * Dequote a filename, if it's quoted.
 * completion_charp must point to escape character or '\0', as per
 * comments for complete_from_files().
 */
static char *
dequote_file_name(char *fname, int quote_char)
{
	char	   *unquoted_fname;

	/*
	 * If quote_char is set, it's not included in "fname".  We have to add it
	 * or strtokx will not interpret the string correctly (notably, it won't
	 * recognize escapes).
	 */
	if (quote_char == '\'')
	{
		char	   *workspace = (char *) pg_malloc(strlen(fname) + 2);

		workspace[0] = quote_char;
		strcpy(workspace + 1, fname);
		unquoted_fname = strtokx(workspace, "", NULL, "'", *completion_charp,
								 false, true, pset.encoding);
		free(workspace);
	}
	else
		unquoted_fname = strtokx(fname, "", NULL, "'", *completion_charp,
								 false, true, pset.encoding);

	/* expect a NULL return for the empty string only */
	if (!unquoted_fname)
	{
		Assert(*fname == '\0');
		unquoted_fname = fname;
	}

	/* readline expects a malloc'd result that it is to free */
	return pg_strdup(unquoted_fname);
}

#endif							/* USE_FILENAME_QUOTING_FUNCTIONS */

#endif							/* USE_READLINE */
