/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
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
#include "tab-complete.h"
#include "input.h"

/* If we don't have this, we might as well forget about the whole thing: */
#ifdef USE_READLINE

#include <ctype.h>
#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "common.h"
#include "settings.h"
#include "stringutils.h"

#ifdef HAVE_RL_FILENAME_COMPLETION_FUNCTION
#define filename_completion_function rl_filename_completion_function
#else
/* missing in some header files */
extern char *filename_completion_function();
#endif

#ifdef HAVE_RL_COMPLETION_MATCHES
#define completion_matches rl_completion_matches
#endif

/* word break characters */
#define WORD_BREAKS		"\t\n@$><=;|&{() "

/*
 * Since readline doesn't let us pass any state through to the tab completion
 * callback, we have to use this global variable to let get_previous_words()
 * get at the previous lines of the current command.  Ick.
 */
PQExpBuffer tab_completion_query_buf = NULL;

/*
 * This struct is used to define "schema queries", which are custom-built
 * to obtain possibly-schema-qualified names of database objects.  There is
 * enough similarity in the structure that we don't want to repeat it each
 * time.  So we put the components of each query into this struct and
 * assemble them with the common boilerplate in _complete_from_query().
 */
typedef struct SchemaQuery
{
	/*
	 * Name of catalog or catalogs to be queried, with alias, eg.
	 * "pg_catalog.pg_class c".  Note that "pg_namespace n" will be added.
	 */
	const char *catname;

	/*
	 * Selection condition --- only rows meeting this condition are candidates
	 * to display.  If catname mentions multiple tables, include the necessary
	 * join condition here.  For example, "c.relkind = 'r'". Write NULL (not
	 * an empty string) if not needed.
	 */
	const char *selcondition;

	/*
	 * Visibility condition --- which rows are visible without schema
	 * qualification?  For example, "pg_catalog.pg_table_is_visible(c.oid)".
	 */
	const char *viscondition;

	/*
	 * Namespace --- name of field to join to pg_namespace.oid. For example,
	 * "c.relnamespace".
	 */
	const char *namespace;

	/*
	 * Result --- the appropriately-quoted name to return, in the case of an
	 * unqualified name.  For example, "pg_catalog.quote_ident(c.relname)".
	 */
	const char *result;

	/*
	 * In some cases a different result must be used for qualified names.
	 * Enter that here, or write NULL if result can be used.
	 */
	const char *qualresult;
} SchemaQuery;


/* Store maximum number of records we want from database queries
 * (implemented via SELECT ... LIMIT xx).
 */
static int	completion_max_records;

/*
 * Communication variables set by COMPLETE_WITH_FOO macros and then used by
 * the completion callback functions.  Ugly but there is no better way.
 */
static const char *completion_charp;	/* to pass a string */
static const char *const * completion_charpp;	/* to pass a list of strings */
static const char *completion_info_charp;		/* to pass a second string */
static const char *completion_info_charp2;		/* to pass a third string */
static const SchemaQuery *completion_squery;	/* to pass a SchemaQuery */
static bool completion_case_sensitive;	/* completion is case sensitive */

/*
 * A few macros to ease typing. You can use these to complete the given
 * string with
 * 1) The results from a query you pass it. (Perhaps one of those below?)
 * 2) The results from a schema query you pass it.
 * 3) The items from a null-pointer-terminated list (with or without
 *	  case-sensitive comparison; see also COMPLETE_WITH_LISTn, below).
 * 4) A string constant.
 * 5) The list of attributes of the given table (possibly schema-qualified).
 * 6/ The list of arguments to the given function (possibly schema-qualified).
 */
#define COMPLETE_WITH_QUERY(query) \
do { \
	completion_charp = query; \
	matches = completion_matches(text, complete_from_query); \
} while (0)

#define COMPLETE_WITH_SCHEMA_QUERY(query, addon) \
do { \
	completion_squery = &(query); \
	completion_charp = addon; \
	matches = completion_matches(text, complete_from_schema_query); \
} while (0)

#define COMPLETE_WITH_LIST_CS(list) \
do { \
	completion_charpp = list; \
	completion_case_sensitive = true; \
	matches = completion_matches(text, complete_from_list); \
} while (0)

#define COMPLETE_WITH_LIST(list) \
do { \
	completion_charpp = list; \
	completion_case_sensitive = false; \
	matches = completion_matches(text, complete_from_list); \
} while (0)

#define COMPLETE_WITH_CONST(string) \
do { \
	completion_charp = string; \
	completion_case_sensitive = false; \
	matches = completion_matches(text, complete_from_const); \
} while (0)

#define COMPLETE_WITH_ATTR(relation, addon) \
do { \
	char   *_completion_schema; \
	char   *_completion_table; \
\
	_completion_schema = strtokx(relation, " \t\n\r", ".", "\"", 0, \
								 false, false, pset.encoding); \
	(void) strtokx(NULL, " \t\n\r", ".", "\"", 0, \
				   false, false, pset.encoding); \
	_completion_table = strtokx(NULL, " \t\n\r", ".", "\"", 0, \
								false, false, pset.encoding); \
	if (_completion_table == NULL) \
	{ \
		completion_charp = Query_for_list_of_attributes  addon; \
		completion_info_charp = relation; \
	} \
	else \
	{ \
		completion_charp = Query_for_list_of_attributes_with_schema  addon; \
		completion_info_charp = _completion_table; \
		completion_info_charp2 = _completion_schema; \
	} \
	matches = completion_matches(text, complete_from_query); \
} while (0)

#define COMPLETE_WITH_FUNCTION_ARG(function) \
do { \
	char   *_completion_schema; \
	char   *_completion_function; \
\
	_completion_schema = strtokx(function, " \t\n\r", ".", "\"", 0, \
								 false, false, pset.encoding); \
	(void) strtokx(NULL, " \t\n\r", ".", "\"", 0, \
				   false, false, pset.encoding); \
	_completion_function = strtokx(NULL, " \t\n\r", ".", "\"", 0, \
								   false, false, pset.encoding); \
	if (_completion_function == NULL) \
	{ \
		completion_charp = Query_for_list_of_arguments; \
		completion_info_charp = function; \
	} \
	else \
	{ \
		completion_charp = Query_for_list_of_arguments_with_schema; \
		completion_info_charp = _completion_function; \
		completion_info_charp2 = _completion_schema; \
	} \
	matches = completion_matches(text, complete_from_query); \
} while (0)

/*
 * These macros simplify use of COMPLETE_WITH_LIST for short, fixed lists.
 * There is no COMPLETE_WITH_LIST1; use COMPLETE_WITH_CONST for that case.
 */
#define COMPLETE_WITH_LIST2(s1, s2) \
do { \
	static const char *const list[] = { s1, s2, NULL }; \
	COMPLETE_WITH_LIST(list); \
} while (0)

#define COMPLETE_WITH_LIST3(s1, s2, s3) \
do { \
	static const char *const list[] = { s1, s2, s3, NULL }; \
	COMPLETE_WITH_LIST(list); \
} while (0)

#define COMPLETE_WITH_LIST4(s1, s2, s3, s4) \
do { \
	static const char *const list[] = { s1, s2, s3, s4, NULL }; \
	COMPLETE_WITH_LIST(list); \
} while (0)

#define COMPLETE_WITH_LIST5(s1, s2, s3, s4, s5) \
do { \
	static const char *const list[] = { s1, s2, s3, s4, s5, NULL }; \
	COMPLETE_WITH_LIST(list); \
} while (0)

#define COMPLETE_WITH_LIST6(s1, s2, s3, s4, s5, s6) \
do { \
	static const char *const list[] = { s1, s2, s3, s4, s5, s6, NULL }; \
	COMPLETE_WITH_LIST(list); \
} while (0)

#define COMPLETE_WITH_LIST7(s1, s2, s3, s4, s5, s6, s7) \
do { \
	static const char *const list[] = { s1, s2, s3, s4, s5, s6, s7, NULL }; \
	COMPLETE_WITH_LIST(list); \
} while (0)

#define COMPLETE_WITH_LIST8(s1, s2, s3, s4, s5, s6, s7, s8) \
do { \
	static const char *const list[] = { s1, s2, s3, s4, s5, s6, s7, s8, NULL }; \
	COMPLETE_WITH_LIST(list); \
} while (0)

#define COMPLETE_WITH_LIST9(s1, s2, s3, s4, s5, s6, s7, s8, s9) \
do { \
	static const char *const list[] = { s1, s2, s3, s4, s5, s6, s7, s8, s9, NULL }; \
	COMPLETE_WITH_LIST(list); \
} while (0)

#define COMPLETE_WITH_LIST10(s1, s2, s3, s4, s5, s6, s7, s8, s9, s10) \
do { \
	static const char *const list[] = { s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, NULL }; \
	COMPLETE_WITH_LIST(list); \
} while (0)

/*
 * Likewise for COMPLETE_WITH_LIST_CS.
 */
#define COMPLETE_WITH_LIST_CS2(s1, s2) \
do { \
	static const char *const list[] = { s1, s2, NULL }; \
	COMPLETE_WITH_LIST_CS(list); \
} while (0)

#define COMPLETE_WITH_LIST_CS3(s1, s2, s3) \
do { \
	static const char *const list[] = { s1, s2, s3, NULL }; \
	COMPLETE_WITH_LIST_CS(list); \
} while (0)

#define COMPLETE_WITH_LIST_CS4(s1, s2, s3, s4) \
do { \
	static const char *const list[] = { s1, s2, s3, s4, NULL }; \
	COMPLETE_WITH_LIST_CS(list); \
} while (0)

#define COMPLETE_WITH_LIST_CS5(s1, s2, s3, s4, s5) \
do { \
	static const char *const list[] = { s1, s2, s3, s4, s5, NULL }; \
	COMPLETE_WITH_LIST_CS(list); \
} while (0)

/*
 * Assembly instructions for schema queries
 */

static const SchemaQuery Query_for_list_of_aggregates = {
	/* catname */
	"pg_catalog.pg_proc p",
	/* selcondition */
	"p.proisagg",
	/* viscondition */
	"pg_catalog.pg_function_is_visible(p.oid)",
	/* namespace */
	"p.pronamespace",
	/* result */
	"pg_catalog.quote_ident(p.proname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_datatypes = {
	/* catname */
	"pg_catalog.pg_type t",
	/* selcondition --- ignore table rowtypes and array types */
	"(t.typrelid = 0 "
	" OR (SELECT c.relkind = 'c' FROM pg_catalog.pg_class c WHERE c.oid = t.typrelid)) "
	"AND t.typname !~ '^_'",
	/* viscondition */
	"pg_catalog.pg_type_is_visible(t.oid)",
	/* namespace */
	"t.typnamespace",
	/* result */
	"pg_catalog.format_type(t.oid, NULL)",
	/* qualresult */
	"pg_catalog.quote_ident(t.typname)"
};

static const SchemaQuery Query_for_list_of_domains = {
	/* catname */
	"pg_catalog.pg_type t",
	/* selcondition */
	"t.typtype = 'd'",
	/* viscondition */
	"pg_catalog.pg_type_is_visible(t.oid)",
	/* namespace */
	"t.typnamespace",
	/* result */
	"pg_catalog.quote_ident(t.typname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_functions = {
	/* catname */
	"pg_catalog.pg_proc p",
	/* selcondition */
	NULL,
	/* viscondition */
	"pg_catalog.pg_function_is_visible(p.oid)",
	/* namespace */
	"p.pronamespace",
	/* result */
	"pg_catalog.quote_ident(p.proname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_indexes = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('i')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_sequences = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('S')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_foreign_tables = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('f')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_tables = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('r')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_constraints_with_schema = {
	/* catname */
	"pg_catalog.pg_constraint c",
	/* selcondition */
	"c.conrelid <> 0",
	/* viscondition */
	"true",						/* there is no pg_constraint_is_visible */
	/* namespace */
	"c.connamespace",
	/* result */
	"pg_catalog.quote_ident(c.conname)",
	/* qualresult */
	NULL
};

/* Relations supporting INSERT, UPDATE or DELETE */
static const SchemaQuery Query_for_list_of_updatables = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('r', 'f', 'v')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_relations = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	NULL,
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_tsvmf = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('r', 'S', 'v', 'm', 'f')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_tmf = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('r', 'm', 'f')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_tm = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('r', 'm')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_views = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('v')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_matviews = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('m')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};


/*
 * Queries to get lists of names of various kinds of things, possibly
 * restricted to names matching a partially entered name.  In these queries,
 * the first %s will be replaced by the text entered so far (suitably escaped
 * to become a SQL literal string).  %d will be replaced by the length of the
 * string (in unescaped form).  A second and third %s, if present, will be
 * replaced by a suitably-escaped version of the string provided in
 * completion_info_charp.  A fourth and fifth %s are similarly replaced by
 * completion_info_charp2.
 *
 * Beware that the allowed sequences of %s and %d are determined by
 * _complete_from_query().
 */

#define Query_for_list_of_attributes \
"SELECT pg_catalog.quote_ident(attname) "\
"  FROM pg_catalog.pg_attribute a, pg_catalog.pg_class c "\
" WHERE c.oid = a.attrelid "\
"   AND a.attnum > 0 "\
"   AND NOT a.attisdropped "\
"   AND substring(pg_catalog.quote_ident(attname),1,%d)='%s' "\
"   AND (pg_catalog.quote_ident(relname)='%s' "\
"        OR '\"' || relname || '\"'='%s') "\
"   AND pg_catalog.pg_table_is_visible(c.oid)"

#define Query_for_list_of_attributes_with_schema \
"SELECT pg_catalog.quote_ident(attname) "\
"  FROM pg_catalog.pg_attribute a, pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
" WHERE c.oid = a.attrelid "\
"   AND n.oid = c.relnamespace "\
"   AND a.attnum > 0 "\
"   AND NOT a.attisdropped "\
"   AND substring(pg_catalog.quote_ident(attname),1,%d)='%s' "\
"   AND (pg_catalog.quote_ident(relname)='%s' "\
"        OR '\"' || relname || '\"' ='%s') "\
"   AND (pg_catalog.quote_ident(nspname)='%s' "\
"        OR '\"' || nspname || '\"' ='%s') "

#define Query_for_list_of_template_databases \
"SELECT pg_catalog.quote_ident(datname) FROM pg_catalog.pg_database "\
" WHERE substring(pg_catalog.quote_ident(datname),1,%d)='%s' AND datistemplate"

#define Query_for_list_of_databases \
"SELECT pg_catalog.quote_ident(datname) FROM pg_catalog.pg_database "\
" WHERE substring(pg_catalog.quote_ident(datname),1,%d)='%s'"

#define Query_for_list_of_tablespaces \
"SELECT pg_catalog.quote_ident(spcname) FROM pg_catalog.pg_tablespace "\
" WHERE substring(pg_catalog.quote_ident(spcname),1,%d)='%s'"

#define Query_for_list_of_encodings \
" SELECT DISTINCT pg_catalog.pg_encoding_to_char(conforencoding) "\
"   FROM pg_catalog.pg_conversion "\
"  WHERE substring(pg_catalog.pg_encoding_to_char(conforencoding),1,%d)=UPPER('%s')"

#define Query_for_list_of_languages \
"SELECT pg_catalog.quote_ident(lanname) "\
"  FROM pg_catalog.pg_language "\
" WHERE lanname != 'internal' "\
"   AND substring(pg_catalog.quote_ident(lanname),1,%d)='%s'"

#define Query_for_list_of_schemas \
"SELECT pg_catalog.quote_ident(nspname) FROM pg_catalog.pg_namespace "\
" WHERE substring(pg_catalog.quote_ident(nspname),1,%d)='%s'"

#define Query_for_list_of_alter_system_set_vars \
"SELECT name FROM "\
" (SELECT pg_catalog.lower(name) AS name FROM pg_catalog.pg_settings "\
"  WHERE context != 'internal') ss "\
" WHERE substring(name,1,%d)='%s'"\
" UNION ALL SELECT 'all' ss"

#define Query_for_list_of_set_vars \
"SELECT name FROM "\
" (SELECT pg_catalog.lower(name) AS name FROM pg_catalog.pg_settings "\
"  WHERE context IN ('user', 'superuser') "\
"  UNION ALL SELECT 'constraints' "\
"  UNION ALL SELECT 'transaction' "\
"  UNION ALL SELECT 'session' "\
"  UNION ALL SELECT 'role' "\
"  UNION ALL SELECT 'tablespace' "\
"  UNION ALL SELECT 'all') ss "\
" WHERE substring(name,1,%d)='%s'"

#define Query_for_list_of_show_vars \
"SELECT name FROM "\
" (SELECT pg_catalog.lower(name) AS name FROM pg_catalog.pg_settings "\
"  UNION ALL SELECT 'session authorization' "\
"  UNION ALL SELECT 'all') ss "\
" WHERE substring(name,1,%d)='%s'"

#define Query_for_list_of_roles \
" SELECT pg_catalog.quote_ident(rolname) "\
"   FROM pg_catalog.pg_roles "\
"  WHERE substring(pg_catalog.quote_ident(rolname),1,%d)='%s'"

#define Query_for_list_of_grant_roles \
" SELECT pg_catalog.quote_ident(rolname) "\
"   FROM pg_catalog.pg_roles "\
"  WHERE substring(pg_catalog.quote_ident(rolname),1,%d)='%s'"\
" UNION ALL SELECT 'PUBLIC'"\
" UNION ALL SELECT 'CURRENT_USER'"\
" UNION ALL SELECT 'SESSION_USER'"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_table_owning_index \
"SELECT pg_catalog.quote_ident(c1.relname) "\
"  FROM pg_catalog.pg_class c1, pg_catalog.pg_class c2, pg_catalog.pg_index i"\
" WHERE c1.oid=i.indrelid and i.indexrelid=c2.oid"\
"       and (%d = pg_catalog.length('%s'))"\
"       and pg_catalog.quote_ident(c2.relname)='%s'"\
"       and pg_catalog.pg_table_is_visible(c2.oid)"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_index_of_table \
"SELECT pg_catalog.quote_ident(c2.relname) "\
"  FROM pg_catalog.pg_class c1, pg_catalog.pg_class c2, pg_catalog.pg_index i"\
" WHERE c1.oid=i.indrelid and i.indexrelid=c2.oid"\
"       and (%d = pg_catalog.length('%s'))"\
"       and pg_catalog.quote_ident(c1.relname)='%s'"\
"       and pg_catalog.pg_table_is_visible(c2.oid)"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_constraint_of_table \
"SELECT pg_catalog.quote_ident(conname) "\
"  FROM pg_catalog.pg_class c1, pg_catalog.pg_constraint con "\
" WHERE c1.oid=conrelid and (%d = pg_catalog.length('%s'))"\
"       and pg_catalog.quote_ident(c1.relname)='%s'"\
"       and pg_catalog.pg_table_is_visible(c1.oid)"

#define Query_for_all_table_constraints \
"SELECT pg_catalog.quote_ident(conname) "\
"  FROM pg_catalog.pg_constraint c "\
" WHERE c.conrelid <> 0 "

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_constraint_of_type \
"SELECT pg_catalog.quote_ident(conname) "\
"  FROM pg_catalog.pg_type t, pg_catalog.pg_constraint con "\
" WHERE t.oid=contypid and (%d = pg_catalog.length('%s'))"\
"       and pg_catalog.quote_ident(t.typname)='%s'"\
"       and pg_catalog.pg_type_is_visible(t.oid)"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_list_of_tables_for_constraint \
"SELECT pg_catalog.quote_ident(relname) "\
"  FROM pg_catalog.pg_class"\
" WHERE (%d = pg_catalog.length('%s'))"\
"   AND oid IN "\
"       (SELECT conrelid FROM pg_catalog.pg_constraint "\
"         WHERE pg_catalog.quote_ident(conname)='%s')"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_rule_of_table \
"SELECT pg_catalog.quote_ident(rulename) "\
"  FROM pg_catalog.pg_class c1, pg_catalog.pg_rewrite "\
" WHERE c1.oid=ev_class and (%d = pg_catalog.length('%s'))"\
"       and pg_catalog.quote_ident(c1.relname)='%s'"\
"       and pg_catalog.pg_table_is_visible(c1.oid)"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_list_of_tables_for_rule \
"SELECT pg_catalog.quote_ident(relname) "\
"  FROM pg_catalog.pg_class"\
" WHERE (%d = pg_catalog.length('%s'))"\
"   AND oid IN "\
"       (SELECT ev_class FROM pg_catalog.pg_rewrite "\
"         WHERE pg_catalog.quote_ident(rulename)='%s')"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_trigger_of_table \
"SELECT pg_catalog.quote_ident(tgname) "\
"  FROM pg_catalog.pg_class c1, pg_catalog.pg_trigger "\
" WHERE c1.oid=tgrelid and (%d = pg_catalog.length('%s'))"\
"       and pg_catalog.quote_ident(c1.relname)='%s'"\
"       and pg_catalog.pg_table_is_visible(c1.oid)"\
"       and not tgisinternal"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_list_of_tables_for_trigger \
"SELECT pg_catalog.quote_ident(relname) "\
"  FROM pg_catalog.pg_class"\
" WHERE (%d = pg_catalog.length('%s'))"\
"   AND oid IN "\
"       (SELECT tgrelid FROM pg_catalog.pg_trigger "\
"         WHERE pg_catalog.quote_ident(tgname)='%s')"

#define Query_for_list_of_ts_configurations \
"SELECT pg_catalog.quote_ident(cfgname) FROM pg_catalog.pg_ts_config "\
" WHERE substring(pg_catalog.quote_ident(cfgname),1,%d)='%s'"

#define Query_for_list_of_ts_dictionaries \
"SELECT pg_catalog.quote_ident(dictname) FROM pg_catalog.pg_ts_dict "\
" WHERE substring(pg_catalog.quote_ident(dictname),1,%d)='%s'"

#define Query_for_list_of_ts_parsers \
"SELECT pg_catalog.quote_ident(prsname) FROM pg_catalog.pg_ts_parser "\
" WHERE substring(pg_catalog.quote_ident(prsname),1,%d)='%s'"

#define Query_for_list_of_ts_templates \
"SELECT pg_catalog.quote_ident(tmplname) FROM pg_catalog.pg_ts_template "\
" WHERE substring(pg_catalog.quote_ident(tmplname),1,%d)='%s'"

#define Query_for_list_of_fdws \
" SELECT pg_catalog.quote_ident(fdwname) "\
"   FROM pg_catalog.pg_foreign_data_wrapper "\
"  WHERE substring(pg_catalog.quote_ident(fdwname),1,%d)='%s'"

#define Query_for_list_of_servers \
" SELECT pg_catalog.quote_ident(srvname) "\
"   FROM pg_catalog.pg_foreign_server "\
"  WHERE substring(pg_catalog.quote_ident(srvname),1,%d)='%s'"

#define Query_for_list_of_user_mappings \
" SELECT pg_catalog.quote_ident(usename) "\
"   FROM pg_catalog.pg_user_mappings "\
"  WHERE substring(pg_catalog.quote_ident(usename),1,%d)='%s'"

#define Query_for_list_of_access_methods \
" SELECT pg_catalog.quote_ident(amname) "\
"   FROM pg_catalog.pg_am "\
"  WHERE substring(pg_catalog.quote_ident(amname),1,%d)='%s'"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_list_of_arguments \
"SELECT pg_catalog.oidvectortypes(proargtypes)||')' "\
"  FROM pg_catalog.pg_proc "\
" WHERE (%d = pg_catalog.length('%s'))"\
"   AND (pg_catalog.quote_ident(proname)='%s'"\
"        OR '\"' || proname || '\"'='%s') "\
"   AND (pg_catalog.pg_function_is_visible(pg_proc.oid))"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_list_of_arguments_with_schema \
"SELECT pg_catalog.oidvectortypes(proargtypes)||')' "\
"  FROM pg_catalog.pg_proc p, pg_catalog.pg_namespace n "\
" WHERE (%d = pg_catalog.length('%s'))"\
"   AND n.oid = p.pronamespace "\
"   AND (pg_catalog.quote_ident(proname)='%s' "\
"        OR '\"' || proname || '\"' ='%s') "\
"   AND (pg_catalog.quote_ident(nspname)='%s' "\
"        OR '\"' || nspname || '\"' ='%s') "

#define Query_for_list_of_extensions \
" SELECT pg_catalog.quote_ident(extname) "\
"   FROM pg_catalog.pg_extension "\
"  WHERE substring(pg_catalog.quote_ident(extname),1,%d)='%s'"

#define Query_for_list_of_available_extensions \
" SELECT pg_catalog.quote_ident(name) "\
"   FROM pg_catalog.pg_available_extensions "\
"  WHERE substring(pg_catalog.quote_ident(name),1,%d)='%s' AND installed_version IS NULL"

/* the silly-looking length condition is just to eat up the current word */
#define Query_for_list_of_available_extension_versions \
" SELECT pg_catalog.quote_ident(version) "\
"   FROM pg_catalog.pg_available_extension_versions "\
"  WHERE (%d = pg_catalog.length('%s'))"\
"    AND pg_catalog.quote_ident(name)='%s'"

#define Query_for_list_of_prepared_statements \
" SELECT pg_catalog.quote_ident(name) "\
"   FROM pg_catalog.pg_prepared_statements "\
"  WHERE substring(pg_catalog.quote_ident(name),1,%d)='%s'"

#define Query_for_list_of_event_triggers \
" SELECT pg_catalog.quote_ident(evtname) "\
"   FROM pg_catalog.pg_event_trigger "\
"  WHERE substring(pg_catalog.quote_ident(evtname),1,%d)='%s'"

#define Query_for_list_of_tablesample_methods \
" SELECT pg_catalog.quote_ident(proname) "\
"   FROM pg_catalog.pg_proc "\
"  WHERE prorettype = 'pg_catalog.tsm_handler'::pg_catalog.regtype AND "\
"        proargtypes[0] = 'pg_catalog.internal'::pg_catalog.regtype AND "\
"        substring(pg_catalog.quote_ident(proname),1,%d)='%s'"

#define Query_for_list_of_policies \
" SELECT pg_catalog.quote_ident(polname) "\
"   FROM pg_catalog.pg_policy "\
"  WHERE substring(pg_catalog.quote_ident(polname),1,%d)='%s'"

#define Query_for_list_of_tables_for_policy \
"SELECT pg_catalog.quote_ident(relname) "\
"  FROM pg_catalog.pg_class"\
" WHERE (%d = pg_catalog.length('%s'))"\
"   AND oid IN "\
"       (SELECT polrelid FROM pg_catalog.pg_policy "\
"         WHERE pg_catalog.quote_ident(polname)='%s')"

#define Query_for_enum \
" SELECT name FROM ( "\
"   SELECT pg_catalog.quote_ident(pg_catalog.unnest(enumvals)) AS name "\
"     FROM pg_catalog.pg_settings "\
"    WHERE pg_catalog.lower(name)=pg_catalog.lower('%s') "\
"    UNION ALL " \
"   SELECT 'DEFAULT' ) ss "\
"  WHERE pg_catalog.substring(name,1,%%d)='%%s'"

/*
 * This is a list of all "things" in Pgsql, which can show up after CREATE or
 * DROP; and there is also a query to get a list of them.
 */

typedef struct
{
	const char *name;
	const char *query;			/* simple query, or NULL */
	const SchemaQuery *squery;	/* schema query, or NULL */
	const bits32 flags;			/* visibility flags, see below */
} pgsql_thing_t;

#define THING_NO_CREATE		(1 << 0)	/* should not show up after CREATE */
#define THING_NO_DROP		(1 << 1)	/* should not show up after DROP */
#define THING_NO_SHOW		(THING_NO_CREATE | THING_NO_DROP)

static const pgsql_thing_t words_after_create[] = {
	{"AGGREGATE", NULL, &Query_for_list_of_aggregates},
	{"CAST", NULL, NULL},		/* Casts have complex structures for names, so
								 * skip it */
	{"COLLATION", "SELECT pg_catalog.quote_ident(collname) FROM pg_catalog.pg_collation WHERE collencoding IN (-1, pg_catalog.pg_char_to_encoding(pg_catalog.getdatabaseencoding())) AND substring(pg_catalog.quote_ident(collname),1,%d)='%s'"},

	/*
	 * CREATE CONSTRAINT TRIGGER is not supported here because it is designed
	 * to be used only by pg_dump.
	 */
	{"CONFIGURATION", Query_for_list_of_ts_configurations, NULL, THING_NO_SHOW},
	{"CONVERSION", "SELECT pg_catalog.quote_ident(conname) FROM pg_catalog.pg_conversion WHERE substring(pg_catalog.quote_ident(conname),1,%d)='%s'"},
	{"DATABASE", Query_for_list_of_databases},
	{"DICTIONARY", Query_for_list_of_ts_dictionaries, NULL, THING_NO_SHOW},
	{"DOMAIN", NULL, &Query_for_list_of_domains},
	{"EVENT TRIGGER", NULL, NULL},
	{"EXTENSION", Query_for_list_of_extensions},
	{"FOREIGN DATA WRAPPER", NULL, NULL},
	{"FOREIGN TABLE", NULL, NULL},
	{"FUNCTION", NULL, &Query_for_list_of_functions},
	{"GROUP", Query_for_list_of_roles},
	{"LANGUAGE", Query_for_list_of_languages},
	{"INDEX", NULL, &Query_for_list_of_indexes},
	{"MATERIALIZED VIEW", NULL, &Query_for_list_of_matviews},
	{"OPERATOR", NULL, NULL},	/* Querying for this is probably not such a
								 * good idea. */
	{"OWNED", NULL, NULL, THING_NO_CREATE},		/* for DROP OWNED BY ... */
	{"PARSER", Query_for_list_of_ts_parsers, NULL, THING_NO_SHOW},
	{"POLICY", NULL, NULL},
	{"ROLE", Query_for_list_of_roles},
	{"RULE", "SELECT pg_catalog.quote_ident(rulename) FROM pg_catalog.pg_rules WHERE substring(pg_catalog.quote_ident(rulename),1,%d)='%s'"},
	{"SCHEMA", Query_for_list_of_schemas},
	{"SEQUENCE", NULL, &Query_for_list_of_sequences},
	{"SERVER", Query_for_list_of_servers},
	{"TABLE", NULL, &Query_for_list_of_tables},
	{"TABLESPACE", Query_for_list_of_tablespaces},
	{"TEMP", NULL, NULL, THING_NO_DROP},		/* for CREATE TEMP TABLE ... */
	{"TEMPLATE", Query_for_list_of_ts_templates, NULL, THING_NO_SHOW},
	{"TEXT SEARCH", NULL, NULL},
	{"TRIGGER", "SELECT pg_catalog.quote_ident(tgname) FROM pg_catalog.pg_trigger WHERE substring(pg_catalog.quote_ident(tgname),1,%d)='%s' AND NOT tgisinternal"},
	{"TYPE", NULL, &Query_for_list_of_datatypes},
	{"UNIQUE", NULL, NULL, THING_NO_DROP},		/* for CREATE UNIQUE INDEX ... */
	{"UNLOGGED", NULL, NULL, THING_NO_DROP},	/* for CREATE UNLOGGED TABLE
												 * ... */
	{"USER", Query_for_list_of_roles},
	{"USER MAPPING FOR", NULL, NULL},
	{"VIEW", NULL, &Query_for_list_of_views},
	{NULL}						/* end of list */
};


/* Forward declaration of functions */
static char **psql_completion(const char *text, int start, int end);
static char *create_command_generator(const char *text, int state);
static char *drop_command_generator(const char *text, int state);
static char *complete_from_query(const char *text, int state);
static char *complete_from_schema_query(const char *text, int state);
static char *_complete_from_query(int is_schema_query,
					 const char *text, int state);
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
static PGresult *exec_query(const char *query);

static char **get_previous_words(int point, char **buffer, int *nwords);

static char *get_guctype(const char *varname);

#ifdef NOT_USED
static char *quote_file_name(char *text, int match_type, char *quote_pointer);
static char *dequote_file_name(char *text, char quote_char);
#endif


/*
 * Initialize the readline library for our purposes.
 */
void
initialize_readline(void)
{
	rl_readline_name = (char *) pset.progname;
	rl_attempted_completion_function = psql_completion;

	rl_basic_word_break_characters = WORD_BREAKS;

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
 * Any alternative can end with '*' which is a wild card, i.e., it means
 * match any word that matches the characters so far.  (We do not currently
 * support '*' elsewhere than the end of an alternative.)
 *
 * For readability, callers should use the macros MatchAny and MatchAnyExcept
 * to invoke those two special cases for 'pattern'.  (But '|' and '*' must
 * just be written directly in patterns.)
 */
#define MatchAny  NULL
#define MatchAnyExcept(pattern)  ("!" pattern)

static bool
word_matches_internal(const char *pattern,
					  const char *word,
					  bool case_sensitive)
{
	size_t		wordlen,
				patternlen;

	/* NULL pattern matches anything. */
	if (pattern == NULL)
		return true;

	/* Handle negated patterns from the MatchAnyExcept macro. */
	if (*pattern == '!')
		return !word_matches_internal(pattern + 1, word, case_sensitive);

	/* Else consider each alternative in the pattern. */
	wordlen = strlen(word);
	for (;;)
	{
		const char *c;

		/* Find end of current alternative. */
		c = pattern;
		while (*c != '\0' && *c != '|')
			c++;
		/* Was there a wild card?  (Assumes first alternative is not empty) */
		if (c[-1] == '*')
		{
			/* Yes, wildcard match? */
			patternlen = c - pattern - 1;
			if (wordlen >= patternlen &&
				(case_sensitive ?
				 strncmp(word, pattern, patternlen) == 0 :
				 pg_strncasecmp(word, pattern, patternlen) == 0))
				return true;
		}
		else
		{
			/* No, plain match? */
			patternlen = c - pattern;
			if (wordlen == patternlen &&
				(case_sensitive ?
				 strncmp(word, pattern, wordlen) == 0 :
				 pg_strncasecmp(word, pattern, wordlen) == 0))
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
 * There are enough matching calls below that it seems worth having these two
 * interface routines rather than including a third parameter in every call.
 *
 * word_matches --- match case-insensitively.
 */
static bool
word_matches(const char *pattern, const char *word)
{
	return word_matches_internal(pattern, word, false);
}

/*
 * word_matches_cs --- match case-sensitively.
 */
static bool
word_matches_cs(const char *pattern, const char *word)
{
	return word_matches_internal(pattern, word, true);
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
 * completion_matches() function, so we don't have to worry about it.
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

	/* Macros for matching the last N words before point, case-insensitively. */
#define TailMatches1(p1) \
	(previous_words_count >= 1 && \
	 word_matches(p1, prev_wd))

#define TailMatches2(p2, p1) \
	(previous_words_count >= 2 && \
	 word_matches(p1, prev_wd) && \
	 word_matches(p2, prev2_wd))

#define TailMatches3(p3, p2, p1) \
	(previous_words_count >= 3 && \
	 word_matches(p1, prev_wd) && \
	 word_matches(p2, prev2_wd) && \
	 word_matches(p3, prev3_wd))

#define TailMatches4(p4, p3, p2, p1) \
	(previous_words_count >= 4 && \
	 word_matches(p1, prev_wd) && \
	 word_matches(p2, prev2_wd) && \
	 word_matches(p3, prev3_wd) && \
	 word_matches(p4, prev4_wd))

#define TailMatches5(p5, p4, p3, p2, p1) \
	(previous_words_count >= 5 && \
	 word_matches(p1, prev_wd) && \
	 word_matches(p2, prev2_wd) && \
	 word_matches(p3, prev3_wd) && \
	 word_matches(p4, prev4_wd) && \
	 word_matches(p5, prev5_wd))

#define TailMatches6(p6, p5, p4, p3, p2, p1) \
	(previous_words_count >= 6 && \
	 word_matches(p1, prev_wd) && \
	 word_matches(p2, prev2_wd) && \
	 word_matches(p3, prev3_wd) && \
	 word_matches(p4, prev4_wd) && \
	 word_matches(p5, prev5_wd) && \
	 word_matches(p6, prev6_wd))

#define TailMatches7(p7, p6, p5, p4, p3, p2, p1) \
	(previous_words_count >= 7 && \
	 word_matches(p1, prev_wd) && \
	 word_matches(p2, prev2_wd) && \
	 word_matches(p3, prev3_wd) && \
	 word_matches(p4, prev4_wd) && \
	 word_matches(p5, prev5_wd) && \
	 word_matches(p6, prev6_wd) && \
	 word_matches(p7, prev7_wd))

#define TailMatches8(p8, p7, p6, p5, p4, p3, p2, p1) \
	(previous_words_count >= 8 && \
	 word_matches(p1, prev_wd) && \
	 word_matches(p2, prev2_wd) && \
	 word_matches(p3, prev3_wd) && \
	 word_matches(p4, prev4_wd) && \
	 word_matches(p5, prev5_wd) && \
	 word_matches(p6, prev6_wd) && \
	 word_matches(p7, prev7_wd) && \
	 word_matches(p8, prev8_wd))

#define TailMatches9(p9, p8, p7, p6, p5, p4, p3, p2, p1) \
	(previous_words_count >= 9 && \
	 word_matches(p1, prev_wd) && \
	 word_matches(p2, prev2_wd) && \
	 word_matches(p3, prev3_wd) && \
	 word_matches(p4, prev4_wd) && \
	 word_matches(p5, prev5_wd) && \
	 word_matches(p6, prev6_wd) && \
	 word_matches(p7, prev7_wd) && \
	 word_matches(p8, prev8_wd) && \
	 word_matches(p9, prev9_wd))

	/* Macros for matching the last N words before point, case-sensitively. */
#define TailMatchesCS1(p1) \
	(previous_words_count >= 1 && \
	 word_matches_cs(p1, prev_wd))
#define TailMatchesCS2(p2, p1) \
	(previous_words_count >= 2 && \
	 word_matches_cs(p1, prev_wd) && \
	 word_matches_cs(p2, prev2_wd))

	/*
	 * Macros for matching N words beginning at the start of the line,
	 * case-insensitively.
	 */
#define Matches1(p1) \
	(previous_words_count == 1 && \
	 TailMatches1(p1))
#define Matches2(p1, p2) \
	(previous_words_count == 2 && \
	 TailMatches2(p1, p2))
#define Matches3(p1, p2, p3) \
	(previous_words_count == 3 && \
	 TailMatches3(p1, p2, p3))
#define Matches4(p1, p2, p3, p4) \
	(previous_words_count == 4 && \
	 TailMatches4(p1, p2, p3, p4))
#define Matches5(p1, p2, p3, p4, p5) \
	(previous_words_count == 5 && \
	 TailMatches5(p1, p2, p3, p4, p5))
#define Matches6(p1, p2, p3, p4, p5, p6) \
	(previous_words_count == 6 && \
	 TailMatches6(p1, p2, p3, p4, p5, p6))
#define Matches7(p1, p2, p3, p4, p5, p6, p7) \
	(previous_words_count == 7 && \
	 TailMatches7(p1, p2, p3, p4, p5, p6, p7))
#define Matches8(p1, p2, p3, p4, p5, p6, p7, p8) \
	(previous_words_count == 8 && \
	 TailMatches8(p1, p2, p3, p4, p5, p6, p7, p8))
#define Matches9(p1, p2, p3, p4, p5, p6, p7, p8, p9) \
	(previous_words_count == 9 && \
	 TailMatches9(p1, p2, p3, p4, p5, p6, p7, p8, p9))

	/*
	 * Macros for matching N words at the start of the line, regardless of
	 * what is after them, case-insensitively.
	 */
#define HeadMatches1(p1) \
	(previous_words_count >= 1 && \
	 word_matches(p1, previous_words[previous_words_count - 1]))

#define HeadMatches2(p1, p2) \
	(previous_words_count >= 2 && \
	 word_matches(p1, previous_words[previous_words_count - 1]) && \
	 word_matches(p2, previous_words[previous_words_count - 2]))

#define HeadMatches3(p1, p2, p3) \
	(previous_words_count >= 3 && \
	 word_matches(p1, previous_words[previous_words_count - 1]) && \
	 word_matches(p2, previous_words[previous_words_count - 2]) && \
	 word_matches(p3, previous_words[previous_words_count - 3]))

	/* Known command-starting keywords. */
	static const char *const sql_commands[] = {
		"ABORT", "ALTER", "ANALYZE", "BEGIN", "CHECKPOINT", "CLOSE", "CLUSTER",
		"COMMENT", "COMMIT", "COPY", "CREATE", "DEALLOCATE", "DECLARE",
		"DELETE FROM", "DISCARD", "DO", "DROP", "END", "EXECUTE", "EXPLAIN",
		"FETCH", "GRANT", "IMPORT", "INSERT", "LISTEN", "LOAD", "LOCK",
		"MOVE", "NOTIFY", "PREPARE",
		"REASSIGN", "REFRESH MATERIALIZED VIEW", "REINDEX", "RELEASE",
		"RESET", "REVOKE", "ROLLBACK",
		"SAVEPOINT", "SECURITY LABEL", "SELECT", "SET", "SHOW", "START",
		"TABLE", "TRUNCATE", "UNLISTEN", "UPDATE", "VACUUM", "VALUES", "WITH",
		NULL
	};

	/* psql's backslash commands. */
	static const char *const backslash_commands[] = {
		"\\a", "\\connect", "\\conninfo", "\\C", "\\cd", "\\copy", "\\copyright",
		"\\d", "\\da", "\\db", "\\dc", "\\dC", "\\dd", "\\ddp", "\\dD",
		"\\des", "\\det", "\\deu", "\\dew", "\\dE", "\\df",
		"\\dF", "\\dFd", "\\dFp", "\\dFt", "\\dg", "\\di", "\\dl", "\\dL",
		"\\dm", "\\dn", "\\do", "\\dO", "\\dp", "\\drds", "\\ds", "\\dS",
		"\\dt", "\\dT", "\\dv", "\\du", "\\dx", "\\dy",
		"\\e", "\\echo", "\\ef", "\\encoding", "\\ev",
		"\\f", "\\g", "\\gset", "\\h", "\\help", "\\H", "\\i", "\\ir", "\\l",
		"\\lo_import", "\\lo_export", "\\lo_list", "\\lo_unlink",
		"\\o", "\\p", "\\password", "\\prompt", "\\pset", "\\q", "\\qecho", "\\r",
		"\\s", "\\set", "\\setenv", "\\sf", "\\sv", "\\t", "\\T",
		"\\timing", "\\unset", "\\x", "\\w", "\\watch", "\\z", "\\!", NULL
	};

	(void) end;					/* "end" is not used */

#ifdef HAVE_RL_COMPLETION_APPEND_CHARACTER
	rl_completion_append_character = ' ';
#endif

	/* Clear a few things. */
	completion_charp = NULL;
	completion_charpp = NULL;
	completion_info_charp = NULL;
	completion_info_charp2 = NULL;

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
		else
			matches = complete_from_variables(text, ":", "", true);
	}

	/* If no previous word, suggest one of the basic sql commands */
	else if (previous_words_count == 0)
		COMPLETE_WITH_LIST(sql_commands);

/* CREATE */
	/* complete with something you can create */
	else if (TailMatches1("CREATE"))
		matches = completion_matches(text, create_command_generator);

/* DROP, but not DROP embedded in other commands */
	/* complete with something you can drop */
	else if (Matches1("DROP"))
		matches = completion_matches(text, drop_command_generator);

/* ALTER */

	/* ALTER TABLE */
	else if (Matches2("ALTER", "TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   "UNION SELECT 'ALL IN TABLESPACE'");

	/* ALTER something */
	else if (Matches1("ALTER"))
	{
		static const char *const list_ALTER[] =
		{"AGGREGATE", "COLLATION", "CONVERSION", "DATABASE", "DEFAULT PRIVILEGES", "DOMAIN",
			"EVENT TRIGGER", "EXTENSION", "FOREIGN DATA WRAPPER", "FOREIGN TABLE", "FUNCTION",
			"GROUP", "INDEX", "LANGUAGE", "LARGE OBJECT", "MATERIALIZED VIEW", "OPERATOR",
			"POLICY", "ROLE", "RULE", "SCHEMA", "SERVER", "SEQUENCE", "SYSTEM", "TABLE",
			"TABLESPACE", "TEXT SEARCH", "TRIGGER", "TYPE",
		"USER", "USER MAPPING FOR", "VIEW", NULL};

		COMPLETE_WITH_LIST(list_ALTER);
	}
	/* ALTER TABLE,INDEX,MATERIALIZED VIEW ALL IN TABLESPACE xxx */
	else if (TailMatches4("ALL", "IN", "TABLESPACE", MatchAny))
		COMPLETE_WITH_LIST2("SET TABLESPACE", "OWNED BY");
	/* ALTER TABLE,INDEX,MATERIALIZED VIEW ALL IN TABLESPACE xxx OWNED BY */
	else if (TailMatches6("ALL", "IN", "TABLESPACE", MatchAny, "OWNED", "BY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	/* ALTER TABLE,INDEX,MATERIALIZED VIEW ALL IN TABLESPACE xxx OWNED BY xxx */
	else if (TailMatches7("ALL", "IN", "TABLESPACE", MatchAny, "OWNED", "BY", MatchAny))
		COMPLETE_WITH_CONST("SET TABLESPACE");
	/* ALTER AGGREGATE,FUNCTION <name> */
	else if (Matches3("ALTER", "AGGREGATE|FUNCTION", MatchAny))
		COMPLETE_WITH_CONST("(");
	/* ALTER AGGREGATE,FUNCTION <name> (...) */
	else if (Matches4("ALTER", "AGGREGATE|FUNCTION", MatchAny, MatchAny))
	{
		if (ends_with(prev_wd, ')'))
			COMPLETE_WITH_LIST3("OWNER TO", "RENAME TO", "SET SCHEMA");
		else
			COMPLETE_WITH_FUNCTION_ARG(prev2_wd);
	}

	/* ALTER SCHEMA <name> */
	else if (Matches3("ALTER", "SCHEMA", MatchAny))
		COMPLETE_WITH_LIST2("OWNER TO", "RENAME TO");

	/* ALTER COLLATION <name> */
	else if (Matches3("ALTER", "COLLATION", MatchAny))
		COMPLETE_WITH_LIST3("OWNER TO", "RENAME TO", "SET SCHEMA");

	/* ALTER CONVERSION <name> */
	else if (Matches3("ALTER", "CONVERSION", MatchAny))
		COMPLETE_WITH_LIST3("OWNER TO", "RENAME TO", "SET SCHEMA");

	/* ALTER DATABASE <name> */
	else if (Matches3("ALTER", "DATABASE", MatchAny))
		COMPLETE_WITH_LIST7("RESET", "SET", "OWNER TO", "RENAME TO",
							"IS_TEMPLATE", "ALLOW_CONNECTIONS",
							"CONNECTION LIMIT");

	/* ALTER EVENT TRIGGER */
	else if (Matches3("ALTER", "EVENT", "TRIGGER"))
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);

	/* ALTER EVENT TRIGGER <name> */
	else if (Matches4("ALTER", "EVENT", "TRIGGER", MatchAny))
		COMPLETE_WITH_LIST4("DISABLE", "ENABLE", "OWNER TO", "RENAME TO");

	/* ALTER EVENT TRIGGER <name> ENABLE */
	else if (Matches5("ALTER", "EVENT", "TRIGGER", MatchAny, "ENABLE"))
		COMPLETE_WITH_LIST2("REPLICA", "ALWAYS");

	/* ALTER EXTENSION <name> */
	else if (Matches3("ALTER", "EXTENSION", MatchAny))
		COMPLETE_WITH_LIST4("ADD", "DROP", "UPDATE", "SET SCHEMA");

	/* ALTER FOREIGN */
	else if (Matches2("ALTER", "FOREIGN"))
		COMPLETE_WITH_LIST2("DATA WRAPPER", "TABLE");

	/* ALTER FOREIGN DATA WRAPPER <name> */
	else if (Matches5("ALTER", "FOREIGN", "DATA", "WRAPPER", MatchAny))
		COMPLETE_WITH_LIST5("HANDLER", "VALIDATOR", "OPTIONS", "OWNER TO", "RENAME TO");

	/* ALTER FOREIGN TABLE <name> */
	else if (Matches4("ALTER", "FOREIGN", "TABLE", MatchAny))
	{
		static const char *const list_ALTER_FOREIGN_TABLE[] =
		{"ADD", "ALTER", "DISABLE TRIGGER", "DROP", "ENABLE", "INHERIT",
			"NO INHERIT", "OPTIONS", "OWNER TO", "RENAME", "SET",
		"VALIDATE CONSTRAINT", NULL};

		COMPLETE_WITH_LIST(list_ALTER_FOREIGN_TABLE);
	}

	/* ALTER INDEX */
	else if (Matches2("ALTER", "INDEX"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes,
								   "UNION SELECT 'ALL IN TABLESPACE'");
	/* ALTER INDEX <name> */
	else if (Matches3("ALTER", "INDEX", MatchAny))
		COMPLETE_WITH_LIST4("OWNER TO", "RENAME TO", "SET", "RESET");
	/* ALTER INDEX <name> SET */
	else if (Matches4("ALTER", "INDEX", MatchAny, "SET"))
		COMPLETE_WITH_LIST2("(", "TABLESPACE");
	/* ALTER INDEX <name> RESET */
	else if (Matches4("ALTER", "INDEX", MatchAny, "RESET"))
		COMPLETE_WITH_CONST("(");
	/* ALTER INDEX <foo> SET|RESET ( */
	else if (Matches5("ALTER", "INDEX", MatchAny, "RESET", "("))
		COMPLETE_WITH_LIST3("fillfactor", "fastupdate",
							"gin_pending_list_limit");
	else if (Matches5("ALTER", "INDEX", MatchAny, "SET", "("))
		COMPLETE_WITH_LIST3("fillfactor =", "fastupdate =",
							"gin_pending_list_limit =");

	/* ALTER LANGUAGE <name> */
	else if (Matches3("ALTER", "LANGUAGE", MatchAny))
		COMPLETE_WITH_LIST2("OWNER_TO", "RENAME TO");

	/* ALTER LARGE OBJECT <oid> */
	else if (Matches4("ALTER", "LARGE", "OBJECT", MatchAny))
		COMPLETE_WITH_CONST("OWNER TO");

	/* ALTER MATERIALIZED VIEW */
	else if (Matches3("ALTER", "MATERIALIZED", "VIEW"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews,
								   "UNION SELECT 'ALL IN TABLESPACE'");

	/* ALTER USER,ROLE <name> */
	else if (Matches3("ALTER", "USER|ROLE", MatchAny) &&
			 !TailMatches2("USER", "MAPPING"))
	{
		static const char *const list_ALTERUSER[] =
		{"BYPASSRLS", "CONNECTION LIMIT", "CREATEDB", "CREATEROLE",
			"ENCRYPTED", "INHERIT", "LOGIN", "NOBYPASSRLS",
			"NOCREATEDB", "NOCREATEROLE", "NOINHERIT",
			"NOLOGIN", "NOREPLICATION", "NOSUPERUSER", "PASSWORD", "RENAME TO",
			"REPLICATION", "RESET", "SET", "SUPERUSER", "UNENCRYPTED",
		"VALID UNTIL", "WITH", NULL};

		COMPLETE_WITH_LIST(list_ALTERUSER);
	}

	/* ALTER USER,ROLE <name> WITH */
	else if (Matches4("ALTER", "USER|ROLE", MatchAny, "WITH"))
	{
		/* Similar to the above, but don't complete "WITH" again. */
		static const char *const list_ALTERUSER_WITH[] =
		{"BYPASSRLS", "CONNECTION LIMIT", "CREATEDB", "CREATEROLE",
			"ENCRYPTED", "INHERIT", "LOGIN", "NOBYPASSRLS",
			"NOCREATEDB", "NOCREATEROLE", "NOINHERIT",
			"NOLOGIN", "NOREPLICATION", "NOSUPERUSER", "PASSWORD", "RENAME TO",
			"REPLICATION", "RESET", "SET", "SUPERUSER", "UNENCRYPTED",
		"VALID UNTIL", NULL};

		COMPLETE_WITH_LIST(list_ALTERUSER_WITH);
	}

	/* complete ALTER USER,ROLE <name> ENCRYPTED,UNENCRYPTED with PASSWORD */
	else if (Matches4("ALTER", "USER|ROLE", MatchAny, "ENCRYPTED|UNENCRYPTED"))
		COMPLETE_WITH_CONST("PASSWORD");
	/* ALTER DEFAULT PRIVILEGES */
	else if (Matches3("ALTER", "DEFAULT", "PRIVILEGES"))
		COMPLETE_WITH_LIST3("FOR ROLE", "FOR USER", "IN SCHEMA");
	/* ALTER DEFAULT PRIVILEGES FOR */
	else if (Matches4("ALTER", "DEFAULT", "PRIVILEGES", "FOR"))
		COMPLETE_WITH_LIST2("ROLE", "USER");
	/* ALTER DEFAULT PRIVILEGES { FOR ROLE ... | IN SCHEMA ... } */
	else if (Matches6("ALTER", "DEFAULT", "PRIVILEGES", "FOR", "ROLE|USER", MatchAny) ||
		Matches6("ALTER", "DEFAULT", "PRIVILEGES", "IN", "SCHEMA", MatchAny))
		COMPLETE_WITH_LIST2("GRANT", "REVOKE");
	/* ALTER DOMAIN <name> */
	else if (Matches3("ALTER", "DOMAIN", MatchAny))
		COMPLETE_WITH_LIST6("ADD", "DROP", "OWNER TO", "RENAME", "SET",
							"VALIDATE CONSTRAINT");
	/* ALTER DOMAIN <sth> DROP */
	else if (Matches4("ALTER", "DOMAIN", MatchAny, "DROP"))
		COMPLETE_WITH_LIST3("CONSTRAINT", "DEFAULT", "NOT NULL");
	/* ALTER DOMAIN <sth> DROP|RENAME|VALIDATE CONSTRAINT */
	else if (Matches5("ALTER", "DOMAIN", MatchAny, "DROP|RENAME|VALIDATE", "CONSTRAINT"))
	{
		completion_info_charp = prev3_wd;
		COMPLETE_WITH_QUERY(Query_for_constraint_of_type);
	}
	/* ALTER DOMAIN <sth> RENAME */
	else if (Matches4("ALTER", "DOMAIN", MatchAny, "RENAME"))
		COMPLETE_WITH_LIST2("CONSTRAINT", "TO");
	/* ALTER DOMAIN <sth> RENAME CONSTRAINT <sth> */
	else if (Matches6("ALTER", "DOMAIN", MatchAny, "RENAME", "CONSTRAINT", MatchAny))
		COMPLETE_WITH_CONST("TO");

	/* ALTER DOMAIN <sth> SET */
	else if (Matches4("ALTER", "DOMAIN", MatchAny, "SET"))
		COMPLETE_WITH_LIST3("DEFAULT", "NOT NULL", "SCHEMA");
	/* ALTER SEQUENCE <name> */
	else if (Matches3("ALTER", "SEQUENCE", MatchAny))
	{
		static const char *const list_ALTERSEQUENCE[] =
		{"INCREMENT", "MINVALUE", "MAXVALUE", "RESTART", "NO", "CACHE", "CYCLE",
		"SET SCHEMA", "OWNED BY", "OWNER TO", "RENAME TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERSEQUENCE);
	}
	/* ALTER SEQUENCE <name> NO */
	else if (Matches4("ALTER", "SEQUENCE", MatchAny, "NO"))
		COMPLETE_WITH_LIST3("MINVALUE", "MAXVALUE", "CYCLE");
	/* ALTER SERVER <name> */
	else if (Matches3("ALTER", "SERVER", MatchAny))
		COMPLETE_WITH_LIST4("VERSION", "OPTIONS", "OWNER TO", "RENAME TO");
	/* ALTER SERVER <name> VERSION <version>*/
	else if (Matches5("ALTER", "SERVER", MatchAny, "VERSION", MatchAny))
		COMPLETE_WITH_CONST("OPTIONS");
	/* ALTER SYSTEM SET, RESET, RESET ALL */
	else if (Matches2("ALTER", "SYSTEM"))
		COMPLETE_WITH_LIST2("SET", "RESET");
	/* ALTER SYSTEM SET|RESET <name> */
	else if (Matches3("ALTER", "SYSTEM", "SET|RESET"))
		COMPLETE_WITH_QUERY(Query_for_list_of_alter_system_set_vars);
	/* ALTER VIEW <name> */
	else if (Matches3("ALTER", "VIEW", MatchAny))
		COMPLETE_WITH_LIST4("ALTER COLUMN", "OWNER TO", "RENAME TO",
							"SET SCHEMA");
	/* ALTER MATERIALIZED VIEW <name> */
	else if (Matches4("ALTER", "MATERIALIZED", "VIEW", MatchAny))
		COMPLETE_WITH_LIST4("ALTER COLUMN", "OWNER TO", "RENAME TO",
							"SET SCHEMA");

	/* ALTER POLICY <name> */
	else if (Matches2("ALTER", "POLICY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_policies);
	/* ALTER POLICY <name> ON */
	else if (Matches3("ALTER", "POLICY", MatchAny))
		COMPLETE_WITH_CONST("ON");
	/* ALTER POLICY <name> ON <table> */
	else if (Matches4("ALTER", "POLICY", MatchAny, "ON"))
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_tables_for_policy);
	}
	/* ALTER POLICY <name> ON <table> - show options */
	else if (Matches5("ALTER", "POLICY", MatchAny, "ON", MatchAny))
		COMPLETE_WITH_LIST4("RENAME TO", "TO", "USING (", "WITH CHECK (");
	/* ALTER POLICY <name> ON <table> TO <role> */
	else if (Matches6("ALTER", "POLICY", MatchAny, "ON", MatchAny, "TO"))
		COMPLETE_WITH_QUERY(Query_for_list_of_grant_roles);
	/* ALTER POLICY <name> ON <table> USING ( */
	else if (Matches6("ALTER", "POLICY", MatchAny, "ON", MatchAny, "USING"))
		COMPLETE_WITH_CONST("(");
	/* ALTER POLICY <name> ON <table> WITH CHECK ( */
	else if (Matches7("ALTER", "POLICY", MatchAny, "ON", MatchAny, "WITH", "CHECK"))
		COMPLETE_WITH_CONST("(");

	/* ALTER RULE <name>, add ON */
	else if (Matches3("ALTER", "RULE", MatchAny))
		COMPLETE_WITH_CONST("ON");

	/* If we have ALTER RULE <name> ON, then add the correct tablename */
	else if (Matches4("ALTER", "RULE", MatchAny, "ON"))
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_tables_for_rule);
	}

	/* ALTER RULE <name> ON <name> */
	else if (Matches5("ALTER", "RULE", MatchAny, "ON", MatchAny))
		COMPLETE_WITH_CONST("RENAME TO");

	/* ALTER TRIGGER <name>, add ON */
	else if (Matches3("ALTER", "TRIGGER", MatchAny))
		COMPLETE_WITH_CONST("ON");

	else if (Matches4("ALTER", "TRIGGER", MatchAny, MatchAny))
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_tables_for_trigger);
	}

	/*
	 * If we have ALTER TRIGGER <sth> ON, then add the correct tablename
	 */
	else if (Matches4("ALTER", "TRIGGER", MatchAny, "ON"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);

	/* ALTER TRIGGER <name> ON <name> */
	else if (Matches5("ALTER", "TRIGGER", MatchAny, "ON", MatchAny))
		COMPLETE_WITH_CONST("RENAME TO");

	/*
	 * If we detect ALTER TABLE <name>, suggest sub commands
	 */
	else if (Matches3("ALTER", "TABLE", MatchAny))
	{
		static const char *const list_ALTER2[] =
		{"ADD", "ALTER", "CLUSTER ON", "DISABLE", "DROP", "ENABLE", "INHERIT",
			"NO INHERIT", "RENAME", "RESET", "OWNER TO", "SET",
		"VALIDATE CONSTRAINT", "REPLICA IDENTITY", NULL};

		COMPLETE_WITH_LIST(list_ALTER2);
	}
	/* ALTER TABLE xxx ENABLE */
	else if (Matches4("ALTER", "TABLE", MatchAny, "ENABLE"))
		COMPLETE_WITH_LIST5("ALWAYS", "REPLICA", "ROW LEVEL SECURITY", "RULE",
							"TRIGGER");
	else if (Matches5("ALTER", "TABLE", MatchAny, "ENABLE", "REPLICA|ALWAYS"))
		COMPLETE_WITH_LIST2("RULE", "TRIGGER");
	else if (Matches5("ALTER", "TABLE", MatchAny, "ENABLE", "RULE"))
	{
		completion_info_charp = prev3_wd;
		COMPLETE_WITH_QUERY(Query_for_rule_of_table);
	}
	else if (Matches6("ALTER", "TABLE", MatchAny, "ENABLE", MatchAny, "RULE"))
	{
		completion_info_charp = prev4_wd;
		COMPLETE_WITH_QUERY(Query_for_rule_of_table);
	}
	else if (Matches5("ALTER", "TABLE", MatchAny, "ENABLE", "TRIGGER"))
	{
		completion_info_charp = prev3_wd;
		COMPLETE_WITH_QUERY(Query_for_trigger_of_table);
	}
	else if (Matches6("ALTER", "TABLE", MatchAny, "ENABLE", MatchAny, "TRIGGER"))
	{
		completion_info_charp = prev4_wd;
		COMPLETE_WITH_QUERY(Query_for_trigger_of_table);
	}
	/* ALTER TABLE xxx INHERIT */
	else if (Matches4("ALTER", "TABLE", MatchAny, "INHERIT"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, "");
	/* ALTER TABLE xxx NO INHERIT */
	else if (Matches5("ALTER", "TABLE", MatchAny, "NO", "INHERIT"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, "");
	/* ALTER TABLE xxx DISABLE */
	else if (Matches4("ALTER", "TABLE", MatchAny, "DISABLE"))
		COMPLETE_WITH_LIST3("ROW LEVEL SECURITY", "RULE", "TRIGGER");
	else if (Matches5("ALTER", "TABLE", MatchAny, "DISABLE", "RULE"))
	{
		completion_info_charp = prev3_wd;
		COMPLETE_WITH_QUERY(Query_for_rule_of_table);
	}
	else if (Matches5("ALTER", "TABLE", MatchAny, "DISABLE", "TRIGGER"))
	{
		completion_info_charp = prev3_wd;
		COMPLETE_WITH_QUERY(Query_for_trigger_of_table);
	}

	/* ALTER TABLE xxx ALTER */
	else if (Matches4("ALTER", "TABLE", MatchAny, "ALTER"))
		COMPLETE_WITH_ATTR(prev2_wd, " UNION SELECT 'COLUMN' UNION SELECT 'CONSTRAINT'");

	/* ALTER TABLE xxx RENAME */
	else if (Matches4("ALTER", "TABLE", MatchAny, "RENAME"))
		COMPLETE_WITH_ATTR(prev2_wd, " UNION SELECT 'COLUMN' UNION SELECT 'CONSTRAINT' UNION SELECT 'TO'");
	else if (Matches5("ALTER", "TABLE", MatchAny, "ALTER|RENAME", "COLUMN"))
		COMPLETE_WITH_ATTR(prev3_wd, "");

	/* ALTER TABLE xxx RENAME yyy */
	else if (Matches5("ALTER", "TABLE", MatchAny, "RENAME", MatchAnyExcept("CONSTRAINT|TO")))
		COMPLETE_WITH_CONST("TO");

	/* ALTER TABLE xxx RENAME COLUMN/CONSTRAINT yyy */
	else if (Matches6("ALTER", "TABLE", MatchAny, "RENAME", "COLUMN|CONSTRAINT", MatchAnyExcept("TO")))
		COMPLETE_WITH_CONST("TO");

	/* If we have ALTER TABLE <sth> DROP, provide COLUMN or CONSTRAINT */
	else if (Matches4("ALTER", "TABLE", MatchAny, "DROP"))
		COMPLETE_WITH_LIST2("COLUMN", "CONSTRAINT");
	/* If we have ALTER TABLE <sth> DROP COLUMN, provide list of columns */
	else if (Matches5("ALTER", "TABLE", MatchAny, "DROP", "COLUMN"))
		COMPLETE_WITH_ATTR(prev3_wd, "");

	/*
	 * If we have ALTER TABLE <sth> ALTER|DROP|RENAME|VALIDATE CONSTRAINT,
	 * provide list of constraints
	 */
	else if (Matches5("ALTER", "TABLE", MatchAny, "ALTER|DROP|RENAME|VALIDATE", "CONSTRAINT"))
	{
		completion_info_charp = prev3_wd;
		COMPLETE_WITH_QUERY(Query_for_constraint_of_table);
	}
	/* ALTER TABLE ALTER [COLUMN] <foo> */
	else if (Matches6("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny) ||
			 Matches5("ALTER", "TABLE", MatchAny, "ALTER", MatchAny))
		COMPLETE_WITH_LIST4("TYPE", "SET", "RESET", "DROP");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET */
	else if (Matches7("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET") ||
			 Matches6("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET"))
		COMPLETE_WITH_LIST5("(", "DEFAULT", "NOT NULL", "STATISTICS", "STORAGE");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET ( */
	else if (Matches8("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "(") ||
		 Matches7("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET", "("))
		COMPLETE_WITH_LIST2("n_distinct", "n_distinct_inherited");
	/* ALTER TABLE ALTER [COLUMN] <foo> SET STORAGE */
	else if (Matches8("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "SET", "STORAGE") ||
	Matches7("ALTER", "TABLE", MatchAny, "ALTER", MatchAny, "SET", "STORAGE"))
		COMPLETE_WITH_LIST4("PLAIN", "EXTERNAL", "EXTENDED", "MAIN");
	/* ALTER TABLE ALTER [COLUMN] <foo> DROP */
	else if (Matches7("ALTER", "TABLE", MatchAny, "ALTER", "COLUMN", MatchAny, "DROP") ||
			 Matches8("ALTER", "TABLE", MatchAny, "TABLE", MatchAny, "ALTER", MatchAny, "DROP"))
		COMPLETE_WITH_LIST2("DEFAULT", "NOT NULL");
	else if (Matches4("ALTER", "TABLE", MatchAny, "CLUSTER"))
		COMPLETE_WITH_CONST("ON");
	else if (Matches5("ALTER", "TABLE", MatchAny, "CLUSTER", "ON"))
	{
		completion_info_charp = prev3_wd;
		COMPLETE_WITH_QUERY(Query_for_index_of_table);
	}
	/* If we have ALTER TABLE <sth> SET, provide list of attributes and '(' */
	else if (Matches4("ALTER", "TABLE", MatchAny, "SET"))
		COMPLETE_WITH_LIST7("(", "LOGGED", "SCHEMA", "TABLESPACE", "UNLOGGED",
							"WITH", "WITHOUT");

	/*
	 * If we have ALTER TABLE <sth> SET TABLESPACE provide a list of
	 * tablespaces
	 */
	else if (Matches5("ALTER", "TABLE", MatchAny, "SET", "TABLESPACE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
	/* If we have ALTER TABLE <sth> SET WITH provide OIDS */
	else if (Matches5("ALTER", "TABLE", MatchAny, "SET", "WITH"))
		COMPLETE_WITH_CONST("OIDS");
	/* If we have ALTER TABLE <sth> SET WITHOUT provide CLUSTER or OIDS */
	else if (Matches5("ALTER", "TABLE", MatchAny, "SET", "WITHOUT"))
		COMPLETE_WITH_LIST2("CLUSTER", "OIDS");
	/* ALTER TABLE <foo> RESET */
	else if (Matches4("ALTER", "TABLE", MatchAny, "RESET"))
		COMPLETE_WITH_CONST("(");
	/* ALTER TABLE <foo> SET|RESET ( */
	else if (Matches5("ALTER", "TABLE", MatchAny, "SET|RESET", "("))
	{
		static const char *const list_TABLEOPTIONS[] =
		{
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
			"autovacuum_vacuum_scale_factor",
			"autovacuum_vacuum_threshold",
			"fillfactor",
			"log_autovacuum_min_duration",
			"toast.autovacuum_enabled",
			"toast.autovacuum_freeze_max_age",
			"toast.autovacuum_freeze_min_age",
			"toast.autovacuum_freeze_table_age",
			"toast.autovacuum_multixact_freeze_max_age",
			"toast.autovacuum_multixact_freeze_min_age",
			"toast.autovacuum_multixact_freeze_table_age",
			"toast.autovacuum_vacuum_cost_delay",
			"toast.autovacuum_vacuum_cost_limit",
			"toast.autovacuum_vacuum_scale_factor",
			"toast.autovacuum_vacuum_threshold",
			"toast.log_autovacuum_min_duration",
			"user_catalog_table",
			NULL
		};

		COMPLETE_WITH_LIST(list_TABLEOPTIONS);
	}
	else if (Matches7("ALTER", "TABLE", MatchAny, "REPLICA", "IDENTITY", "USING", "INDEX"))
	{
		completion_info_charp = prev5_wd;
		COMPLETE_WITH_QUERY(Query_for_index_of_table);
	}
	else if (Matches6("ALTER", "TABLE", MatchAny, "REPLICA", "IDENTITY", "USING"))
		COMPLETE_WITH_CONST("INDEX");
	else if (Matches5("ALTER", "TABLE", MatchAny, "REPLICA", "IDENTITY"))
		COMPLETE_WITH_LIST4("FULL", "NOTHING", "DEFAULT", "USING");
	else if (Matches4("ALTER", "TABLE", MatchAny, "REPLICA"))
		COMPLETE_WITH_CONST("IDENTITY");

	/* ALTER TABLESPACE <foo> with RENAME TO, OWNER TO, SET, RESET */
	else if (Matches3("ALTER", "TABLESPACE", MatchAny))
		COMPLETE_WITH_LIST4("RENAME TO", "OWNER TO", "SET", "RESET");
	/* ALTER TABLESPACE <foo> SET|RESET */
	else if (Matches4("ALTER", "TABLESPACE", MatchAny, "SET|RESET"))
		COMPLETE_WITH_CONST("(");
	/* ALTER TABLESPACE <foo> SET|RESET ( */
	else if (Matches5("ALTER", "TABLESPACE", MatchAny, "SET|RESET", "("))
		COMPLETE_WITH_LIST3("seq_page_cost", "random_page_cost",
							"effective_io_concurrency");

	/* ALTER TEXT SEARCH */
	else if (Matches3("ALTER", "TEXT", "SEARCH"))
		COMPLETE_WITH_LIST4("CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE");
	else if (Matches5("ALTER", "TEXT", "SEARCH", "TEMPLATE|PARSER", MatchAny))
		COMPLETE_WITH_LIST2("RENAME TO", "SET SCHEMA");
	else if (Matches5("ALTER", "TEXT", "SEARCH", "DICTIONARY", MatchAny))
		COMPLETE_WITH_LIST3("OWNER TO", "RENAME TO", "SET SCHEMA");
	else if (Matches5("ALTER", "TEXT", "SEARCH", "CONFIGURATION", MatchAny))
		COMPLETE_WITH_LIST6("ADD MAPPING FOR", "ALTER MAPPING",
							"DROP MAPPING FOR",
							"OWNER TO", "RENAME TO", "SET SCHEMA");

	/* complete ALTER TYPE <foo> with actions */
	else if (Matches3("ALTER", "TYPE", MatchAny))
		COMPLETE_WITH_LIST7("ADD ATTRIBUTE", "ADD VALUE", "ALTER ATTRIBUTE",
							"DROP ATTRIBUTE",
							"OWNER TO", "RENAME", "SET SCHEMA");
	/* complete ALTER TYPE <foo> ADD with actions */
	else if (Matches4("ALTER", "TYPE", MatchAny, "ADD"))
		COMPLETE_WITH_LIST2("ATTRIBUTE", "VALUE");
	/* ALTER TYPE <foo> RENAME	*/
	else if (Matches4("ALTER", "TYPE", MatchAny, "RENAME"))
		COMPLETE_WITH_LIST2("ATTRIBUTE", "TO");
	/* ALTER TYPE xxx RENAME ATTRIBUTE yyy */
	else if (Matches6("ALTER", "TYPE", MatchAny, "RENAME", "ATTRIBUTE", MatchAny))
		COMPLETE_WITH_CONST("TO");

	/*
	 * If we have ALTER TYPE <sth> ALTER/DROP/RENAME ATTRIBUTE, provide list
	 * of attributes
	 */
	else if (Matches5("ALTER", "TYPE", MatchAny, "ALTER|DROP|RENAME", "ATTRIBUTE"))
		COMPLETE_WITH_ATTR(prev3_wd, "");
	/* ALTER TYPE ALTER ATTRIBUTE <foo> */
	else if (Matches6("ALTER", "TYPE", MatchAny, "ALTER", "ATTRIBUTE", MatchAny))
		COMPLETE_WITH_CONST("TYPE");
	/* complete ALTER GROUP <foo> */
	else if (Matches3("ALTER", "GROUP", MatchAny))
		COMPLETE_WITH_LIST3("ADD USER", "DROP USER", "RENAME TO");
	/* complete ALTER GROUP <foo> ADD|DROP with USER */
	else if (Matches4("ALTER", "GROUP", MatchAny, "ADD|DROP"))
		COMPLETE_WITH_CONST("USER");
	/* complete ALTER GROUP <foo> ADD|DROP USER with a user name */
	else if (Matches5("ALTER", "GROUP", MatchAny, "ADD|DROP", "USER"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);

/* BEGIN, END, ABORT */
	else if (Matches1("BEGIN|END|ABORT"))
		COMPLETE_WITH_LIST2("WORK", "TRANSACTION");
/* COMMIT */
	else if (Matches1("COMMIT"))
		COMPLETE_WITH_LIST3("WORK", "TRANSACTION", "PREPARED");
/* RELEASE SAVEPOINT */
	else if (Matches1("RELEASE"))
		COMPLETE_WITH_CONST("SAVEPOINT");
/* ROLLBACK */
	else if (Matches1("ROLLBACK"))
		COMPLETE_WITH_LIST4("WORK", "TRANSACTION", "TO SAVEPOINT", "PREPARED");
/* CLUSTER */
	else if (Matches1("CLUSTER"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm, "UNION SELECT 'VERBOSE'");
	else if (Matches2("CLUSTER", "VERBOSE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm, NULL);
	/* If we have CLUSTER <sth>, then add "USING" */
	else if (Matches2("CLUSTER", MatchAnyExcept("VERBOSE|ON")))
		COMPLETE_WITH_CONST("USING");
	/* If we have CLUSTER VERBOSE <sth>, then add "USING" */
	else if (Matches3("CLUSTER", "VERBOSE", MatchAny))
		COMPLETE_WITH_CONST("USING");
	/* If we have CLUSTER <sth> USING, then add the index as well */
	else if (Matches3("CLUSTER", MatchAny, "USING") ||
			 Matches4("CLUSTER", "VERBOSE", MatchAny, "USING"))
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_index_of_table);
	}

/* COMMENT */
	else if (Matches1("COMMENT"))
		COMPLETE_WITH_CONST("ON");
	else if (Matches2("COMMENT", "ON"))
	{
		static const char *const list_COMMENT[] =
		{"CAST", "COLLATION", "CONVERSION", "DATABASE", "EVENT TRIGGER", "EXTENSION",
			"FOREIGN DATA WRAPPER", "FOREIGN TABLE",
			"SERVER", "INDEX", "LANGUAGE", "POLICY", "RULE", "SCHEMA", "SEQUENCE",
			"TABLE", "TYPE", "VIEW", "MATERIALIZED VIEW", "COLUMN", "AGGREGATE", "FUNCTION",
			"OPERATOR", "TRIGGER", "CONSTRAINT", "DOMAIN", "LARGE OBJECT",
		"TABLESPACE", "TEXT SEARCH", "ROLE", NULL};

		COMPLETE_WITH_LIST(list_COMMENT);
	}
	else if (Matches3("COMMENT", "ON", "FOREIGN"))
		COMPLETE_WITH_LIST2("DATA WRAPPER", "TABLE");
	else if (Matches4("COMMENT", "ON", "TEXT", "SEARCH"))
		COMPLETE_WITH_LIST4("CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE");
	else if (Matches3("COMMENT", "ON", "CONSTRAINT"))
		COMPLETE_WITH_QUERY(Query_for_all_table_constraints);
	else if (Matches4("COMMENT", "ON", "CONSTRAINT", MatchAny))
		COMPLETE_WITH_CONST("ON");
	else if (Matches5("COMMENT", "ON", "CONSTRAINT", MatchAny, "ON"))
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_tables_for_constraint);
	}
	else if (Matches4("COMMENT", "ON", "MATERIALIZED", "VIEW"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews, NULL);
	else if (Matches4("COMMENT", "ON", "EVENT", "TRIGGER"))
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);
	else if (Matches4("COMMENT", "ON", MatchAny, MatchAnyExcept("IS")) ||
		Matches5("COMMENT", "ON", MatchAny, MatchAny, MatchAnyExcept("IS")) ||
			 Matches6("COMMENT", "ON", MatchAny, MatchAny, MatchAny, MatchAnyExcept("IS")))
		COMPLETE_WITH_CONST("IS");

/* COPY */

	/*
	 * If we have COPY, offer list of tables or "(" (Also cover the analogous
	 * backslash command).
	 */
	else if (Matches1("COPY|\\copy"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   " UNION ALL SELECT '('");
	/* If we have COPY BINARY, complete with list of tables */
	else if (Matches2("COPY", "BINARY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
	/* If we have COPY (, complete it with legal commands */
	else if (Matches2("COPY|\\copy", "("))
		COMPLETE_WITH_LIST7("SELECT", "TABLE", "VALUES", "INSERT", "UPDATE", "DELETE", "WITH");
	/* If we have COPY [BINARY] <sth>, complete it with "TO" or "FROM" */
	else if (Matches2("COPY|\\copy", MatchAny) ||
			 Matches3("COPY", "BINARY", MatchAny))
		COMPLETE_WITH_LIST2("FROM", "TO");
	/* If we have COPY [BINARY] <sth> FROM|TO, complete with filename */
	else if (Matches3("COPY|\\copy", MatchAny, "FROM|TO") ||
			 Matches4("COPY", "BINARY", MatchAny, "FROM|TO"))
	{
		completion_charp = "";
		matches = completion_matches(text, complete_from_files);
	}

	/* Handle COPY [BINARY] <sth> FROM|TO filename */
	else if (Matches4("COPY|\\copy", MatchAny, "FROM|TO", MatchAny) ||
			 Matches5("COPY", "BINARY", MatchAny, "FROM|TO", MatchAny))
		COMPLETE_WITH_LIST6("BINARY", "OIDS", "DELIMITER", "NULL", "CSV",
							"ENCODING");

	/* Handle COPY [BINARY] <sth> FROM|TO filename CSV */
	else if (Matches5("COPY|\\copy", MatchAny, "FROM|TO", MatchAny, "CSV") ||
			 Matches6("COPY", "BINARY", MatchAny, "FROM|TO", MatchAny, "CSV"))
		COMPLETE_WITH_LIST5("HEADER", "QUOTE", "ESCAPE", "FORCE QUOTE",
							"FORCE NOT NULL");

	/* CREATE DATABASE */
	else if (Matches3("CREATE", "DATABASE", MatchAny))
		COMPLETE_WITH_LIST9("OWNER", "TEMPLATE", "ENCODING", "TABLESPACE",
							"IS_TEMPLATE",
							"ALLOW_CONNECTIONS", "CONNECTION LIMIT",
							"LC_COLLATE", "LC_CTYPE");

	else if (Matches4("CREATE", "DATABASE", MatchAny, "TEMPLATE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_template_databases);

	/* CREATE EXTENSION */
	/* Complete with available extensions rather than installed ones. */
	else if (Matches2("CREATE", "EXTENSION"))
		COMPLETE_WITH_QUERY(Query_for_list_of_available_extensions);
	/* CREATE EXTENSION <name> */
	else if (Matches3("CREATE", "EXTENSION", MatchAny))
		COMPLETE_WITH_LIST3("WITH SCHEMA", "CASCADE", "VERSION");
	/* CREATE EXTENSION <name> VERSION */
	else if (Matches4("CREATE", "EXTENSION", MatchAny, "VERSION"))
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_available_extension_versions);
	}

	/* CREATE FOREIGN */
	else if (Matches2("CREATE", "FOREIGN"))
		COMPLETE_WITH_LIST2("DATA WRAPPER", "TABLE");

	/* CREATE FOREIGN DATA WRAPPER */
	else if (Matches5("CREATE", "FOREIGN", "DATA", "WRAPPER", MatchAny))
		COMPLETE_WITH_LIST3("HANDLER", "VALIDATOR", "OPTIONS");

	/* CREATE INDEX --- is allowed inside CREATE SCHEMA, so use TailMatches */
	/* First off we complete CREATE UNIQUE with "INDEX" */
	else if (TailMatches2("CREATE", "UNIQUE"))
		COMPLETE_WITH_CONST("INDEX");
	/* If we have CREATE|UNIQUE INDEX, then add "ON", "CONCURRENTLY",
	   and existing indexes */
	else if (TailMatches2("CREATE|UNIQUE", "INDEX"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes,
								   " UNION SELECT 'ON'"
								   " UNION SELECT 'CONCURRENTLY'");
	/* Complete ... INDEX|CONCURRENTLY [<name>] ON with a list of tables  */
	else if (TailMatches3("INDEX|CONCURRENTLY", MatchAny, "ON") ||
			 TailMatches2("INDEX|CONCURRENTLY", "ON"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm, NULL);
	/* Complete CREATE|UNIQUE INDEX CONCURRENTLY with "ON" and existing indexes */
	else if (TailMatches3("CREATE|UNIQUE", "INDEX", "CONCURRENTLY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes,
								   " UNION SELECT 'ON'");
	/* Complete CREATE|UNIQUE INDEX [CONCURRENTLY] <sth> with "ON" */
	else if (TailMatches3("CREATE|UNIQUE", "INDEX", MatchAny) ||
			 TailMatches4("CREATE|UNIQUE", "INDEX", "CONCURRENTLY", MatchAny))
		COMPLETE_WITH_CONST("ON");

	/*
	 * Complete INDEX <name> ON <table> with a list of table columns (which
	 * should really be in parens)
	 */
	else if (TailMatches4("INDEX", MatchAny, "ON", MatchAny) ||
			 TailMatches3("INDEX|CONCURRENTLY", "ON", MatchAny))
		COMPLETE_WITH_LIST2("(", "USING");
	else if (TailMatches5("INDEX", MatchAny, "ON", MatchAny, "(") ||
			 TailMatches4("INDEX|CONCURRENTLY", "ON", MatchAny, "("))
		COMPLETE_WITH_ATTR(prev2_wd, "");
	/* same if you put in USING */
	else if (TailMatches5("ON", MatchAny, "USING", MatchAny, "("))
		COMPLETE_WITH_ATTR(prev4_wd, "");
	/* Complete USING with an index method */
	else if (TailMatches6("INDEX", MatchAny, MatchAny, "ON", MatchAny, "USING") ||
			 TailMatches5("INDEX", MatchAny, "ON", MatchAny, "USING") ||
			 TailMatches4("INDEX", "ON", MatchAny, "USING"))
		COMPLETE_WITH_QUERY(Query_for_list_of_access_methods);
	else if (TailMatches4("ON", MatchAny, "USING", MatchAny) &&
			 !TailMatches6("POLICY", MatchAny, MatchAny, MatchAny, MatchAny, MatchAny) &&
			 !TailMatches4("FOR", MatchAny, MatchAny, MatchAny))
		COMPLETE_WITH_CONST("(");

	/* CREATE POLICY */
	/* Complete "CREATE POLICY <name> ON" */
	else if (Matches3("CREATE", "POLICY", MatchAny))
		COMPLETE_WITH_CONST("ON");
	/* Complete "CREATE POLICY <name> ON <table>" */
	else if (Matches4("CREATE", "POLICY", MatchAny, "ON"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
	/* Complete "CREATE POLICY <name> ON <table> FOR|TO|USING|WITH CHECK" */
	else if (Matches5("CREATE", "POLICY", MatchAny, "ON", MatchAny))
		COMPLETE_WITH_LIST4("FOR", "TO", "USING (", "WITH CHECK (");
	/* CREATE POLICY <name> ON <table> FOR ALL|SELECT|INSERT|UPDATE|DELETE */
	else if (Matches6("CREATE", "POLICY", MatchAny, "ON", MatchAny, "FOR"))
		COMPLETE_WITH_LIST5("ALL", "SELECT", "INSERT", "UPDATE", "DELETE");
	/* Complete "CREATE POLICY <name> ON <table> FOR INSERT TO|WITH CHECK" */
	else if (Matches7("CREATE", "POLICY", MatchAny, "ON", MatchAny, "FOR", "INSERT"))
		COMPLETE_WITH_LIST2("TO", "WITH CHECK (");
	/* Complete "CREATE POLICY <name> ON <table> FOR SELECT|DELETE TO|USING" */
	else if (Matches7("CREATE", "POLICY", MatchAny, "ON", MatchAny, "FOR", "SELECT|DELETE"))
		COMPLETE_WITH_LIST2("TO", "USING (");
	/* CREATE POLICY <name> ON <table> FOR ALL|UPDATE TO|USING|WITH CHECK */
	else if (Matches7("CREATE", "POLICY", MatchAny, "ON", MatchAny, "FOR", "ALL|UPDATE"))
		COMPLETE_WITH_LIST3("TO", "USING (", "WITH CHECK (");
	/* Complete "CREATE POLICY <name> ON <table> TO <role>" */
	else if (Matches6("CREATE", "POLICY", MatchAny, "ON", MatchAny, "TO"))
		COMPLETE_WITH_QUERY(Query_for_list_of_grant_roles);
	/* Complete "CREATE POLICY <name> ON <table> USING (" */
	else if (Matches6("CREATE", "POLICY", MatchAny, "ON", MatchAny, "USING"))
		COMPLETE_WITH_CONST("(");

/* CREATE RULE */
	/* Complete "CREATE RULE <sth>" with "AS ON" */
	else if (Matches3("CREATE", "RULE", MatchAny))
		COMPLETE_WITH_CONST("AS ON");
	/* Complete "CREATE RULE <sth> AS" with "ON" */
	else if (Matches4("CREATE", "RULE", MatchAny, "AS"))
		COMPLETE_WITH_CONST("ON");
	/* Complete "CREATE RULE <sth> AS ON" with SELECT|UPDATE|INSERT|DELETE */
	else if (Matches5("CREATE", "RULE", MatchAny, "AS", "ON"))
		COMPLETE_WITH_LIST4("SELECT", "UPDATE", "INSERT", "DELETE");
	/* Complete "AS ON SELECT|UPDATE|INSERT|DELETE" with a "TO" */
	else if (TailMatches3("AS", "ON", "SELECT|UPDATE|INSERT|DELETE"))
		COMPLETE_WITH_CONST("TO");
	/* Complete "AS ON <sth> TO" with a table name */
	else if (TailMatches4("AS", "ON", "SELECT|UPDATE|INSERT|DELETE", "TO"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);

/* CREATE SEQUENCE --- is allowed inside CREATE SCHEMA, so use TailMatches */
	else if (TailMatches3("CREATE", "SEQUENCE", MatchAny) ||
			 TailMatches4("CREATE", "TEMP|TEMPORARY", "SEQUENCE", MatchAny))
		COMPLETE_WITH_LIST8("INCREMENT BY", "MINVALUE", "MAXVALUE", "NO", "CACHE",
							"CYCLE", "OWNED BY", "START WITH");
	else if (TailMatches4("CREATE", "SEQUENCE", MatchAny, "NO") ||
		TailMatches5("CREATE", "TEMP|TEMPORARY", "SEQUENCE", MatchAny, "NO"))
		COMPLETE_WITH_LIST3("MINVALUE", "MAXVALUE", "CYCLE");

/* CREATE SERVER <name> */
	else if (Matches3("CREATE", "SERVER", MatchAny))
		COMPLETE_WITH_LIST3("TYPE", "VERSION", "FOREIGN DATA WRAPPER");

/* CREATE TABLE --- is allowed inside CREATE SCHEMA, so use TailMatches */
	/* Complete "CREATE TEMP/TEMPORARY" with the possible temp objects */
	else if (TailMatches2("CREATE", "TEMP|TEMPORARY"))
		COMPLETE_WITH_LIST3("SEQUENCE", "TABLE", "VIEW");
	/* Complete "CREATE UNLOGGED" with TABLE or MATVIEW */
	else if (TailMatches2("CREATE", "UNLOGGED"))
		COMPLETE_WITH_LIST2("TABLE", "MATERIALIZED VIEW");

/* CREATE TABLESPACE */
	else if (Matches3("CREATE", "TABLESPACE", MatchAny))
		COMPLETE_WITH_LIST2("OWNER", "LOCATION");
	/* Complete CREATE TABLESPACE name OWNER name with "LOCATION" */
	else if (Matches5("CREATE", "TABLESPACE", MatchAny, "OWNER", MatchAny))
		COMPLETE_WITH_CONST("LOCATION");

/* CREATE TEXT SEARCH */
	else if (Matches3("CREATE", "TEXT", "SEARCH"))
		COMPLETE_WITH_LIST4("CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE");
	else if (Matches5("CREATE", "TEXT", "SEARCH", "CONFIGURATION", MatchAny))
		COMPLETE_WITH_CONST("(");

/* CREATE TRIGGER --- is allowed inside CREATE SCHEMA, so use TailMatches */
	/* complete CREATE TRIGGER <name> with BEFORE,AFTER,INSTEAD OF */
	else if (TailMatches3("CREATE", "TRIGGER", MatchAny))
		COMPLETE_WITH_LIST3("BEFORE", "AFTER", "INSTEAD OF");
	/* complete CREATE TRIGGER <name> BEFORE,AFTER with an event */
	else if (TailMatches4("CREATE", "TRIGGER", MatchAny, "BEFORE|AFTER"))
		COMPLETE_WITH_LIST4("INSERT", "DELETE", "UPDATE", "TRUNCATE");
	/* complete CREATE TRIGGER <name> INSTEAD OF with an event */
	else if (TailMatches5("CREATE", "TRIGGER", MatchAny, "INSTEAD", "OF"))
		COMPLETE_WITH_LIST3("INSERT", "DELETE", "UPDATE");
	/* complete CREATE TRIGGER <name> BEFORE,AFTER sth with OR,ON */
	else if (TailMatches5("CREATE", "TRIGGER", MatchAny, "BEFORE|AFTER", MatchAny) ||
	  TailMatches6("CREATE", "TRIGGER", MatchAny, "INSTEAD", "OF", MatchAny))
		COMPLETE_WITH_LIST2("ON", "OR");

	/*
	 * complete CREATE TRIGGER <name> BEFORE,AFTER event ON with a list of
	 * tables
	 */
	else if (TailMatches6("CREATE", "TRIGGER", MatchAny, "BEFORE|AFTER", MatchAny, "ON"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
	/* complete CREATE TRIGGER ... INSTEAD OF event ON with a list of views */
	else if (TailMatches7("CREATE", "TRIGGER", MatchAny, "INSTEAD", "OF", MatchAny, "ON"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views, NULL);
	/* complete CREATE TRIGGER ... EXECUTE with PROCEDURE */
	else if (HeadMatches2("CREATE", "TRIGGER") && TailMatches1("EXECUTE"))
		COMPLETE_WITH_CONST("PROCEDURE");

/* CREATE ROLE,USER,GROUP <name> */
	else if (Matches3("CREATE", "ROLE|GROUP|USER", MatchAny) &&
			 !TailMatches2("USER", "MAPPING"))
	{
		static const char *const list_CREATEROLE[] =
		{"ADMIN", "BYPASSRLS", "CONNECTION LIMIT", "CREATEDB", "CREATEROLE",
			"ENCRYPTED", "IN", "INHERIT", "LOGIN", "NOBYPASSRLS",
			"NOCREATEDB", "NOCREATEROLE", "NOINHERIT",
			"NOLOGIN", "NOREPLICATION", "NOSUPERUSER", "PASSWORD",
			"REPLICATION", "ROLE", "SUPERUSER", "SYSID", "UNENCRYPTED",
		"VALID UNTIL", "WITH", NULL};

		COMPLETE_WITH_LIST(list_CREATEROLE);
	}

/* CREATE ROLE,USER,GROUP <name> WITH */
	else if (Matches4("CREATE", "ROLE|GROUP|USER", MatchAny, "WITH"))
	{
		/* Similar to the above, but don't complete "WITH" again. */
		static const char *const list_CREATEROLE_WITH[] =
		{"ADMIN", "BYPASSRLS", "CONNECTION LIMIT", "CREATEDB", "CREATEROLE",
			"ENCRYPTED", "IN", "INHERIT", "LOGIN", "NOBYPASSRLS",
			"NOCREATEDB", "NOCREATEROLE", "NOINHERIT",
			"NOLOGIN", "NOREPLICATION", "NOSUPERUSER", "PASSWORD",
			"REPLICATION", "ROLE", "SUPERUSER", "SYSID", "UNENCRYPTED",
		"VALID UNTIL", NULL};

		COMPLETE_WITH_LIST(list_CREATEROLE_WITH);
	}

	/*
	 * complete CREATE ROLE,USER,GROUP <name> ENCRYPTED,UNENCRYPTED with
	 * PASSWORD
	 */
	else if (Matches4("CREATE", "ROLE|USER|GROUP", MatchAny, "ENCRYPTED|UNENCRYPTED"))
		COMPLETE_WITH_CONST("PASSWORD");
	/* complete CREATE ROLE,USER,GROUP <name> IN with ROLE,GROUP */
	else if (Matches4("CREATE", "ROLE|USER|GROUP", MatchAny, "IN"))
		COMPLETE_WITH_LIST2("GROUP", "ROLE");

/* CREATE VIEW --- is allowed inside CREATE SCHEMA, so use TailMatches */
	/* Complete CREATE VIEW <name> with AS */
	else if (TailMatches3("CREATE", "VIEW", MatchAny))
		COMPLETE_WITH_CONST("AS");
	/* Complete "CREATE VIEW <sth> AS with "SELECT" */
	else if (TailMatches4("CREATE", "VIEW", MatchAny, "AS"))
		COMPLETE_WITH_CONST("SELECT");

/* CREATE MATERIALIZED VIEW */
	else if (Matches2("CREATE", "MATERIALIZED"))
		COMPLETE_WITH_CONST("VIEW");
	/* Complete CREATE MATERIALIZED VIEW <name> with AS */
	else if (Matches4("CREATE", "MATERIALIZED", "VIEW", MatchAny))
		COMPLETE_WITH_CONST("AS");
	/* Complete "CREATE MATERIALIZED VIEW <sth> AS with "SELECT" */
	else if (Matches5("CREATE", "MATERIALIZED", "VIEW", MatchAny, "AS"))
		COMPLETE_WITH_CONST("SELECT");

/* CREATE EVENT TRIGGER */
	else if (Matches2("CREATE", "EVENT"))
		COMPLETE_WITH_CONST("TRIGGER");
	/* Complete CREATE EVENT TRIGGER <name> with ON */
	else if (Matches4("CREATE", "EVENT", "TRIGGER", MatchAny))
		COMPLETE_WITH_CONST("ON");
	/* Complete CREATE EVENT TRIGGER <name> ON with event_type */
	else if (Matches5("CREATE", "EVENT", "TRIGGER", MatchAny, "ON"))
		COMPLETE_WITH_LIST3("ddl_command_start", "ddl_command_end", "sql_drop");

/* DECLARE */
	else if (Matches2("DECLARE", MatchAny))
		COMPLETE_WITH_LIST5("BINARY", "INSENSITIVE", "SCROLL", "NO SCROLL",
							"CURSOR");
	else if (HeadMatches1("DECLARE") && TailMatches1("CURSOR"))
		COMPLETE_WITH_LIST3("WITH HOLD", "WITHOUT HOLD", "FOR");

/* DELETE --- can be inside EXPLAIN, RULE, etc */
	/* ... despite which, only complete DELETE with FROM at start of line */
	else if (Matches1("DELETE"))
		COMPLETE_WITH_CONST("FROM");
	/* Complete DELETE FROM with a list of tables */
	else if (TailMatches2("DELETE", "FROM"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_updatables, NULL);
	/* Complete DELETE FROM <table> */
	else if (TailMatches3("DELETE", "FROM", MatchAny))
		COMPLETE_WITH_LIST2("USING", "WHERE");
	/* XXX: implement tab completion for DELETE ... USING */

/* DISCARD */
	else if (Matches1("DISCARD"))
		COMPLETE_WITH_LIST4("ALL", "PLANS", "SEQUENCES", "TEMP");

/* DO */
	else if (Matches1("DO"))
		COMPLETE_WITH_CONST("LANGUAGE");

/* DROP */
	/* Complete DROP object with CASCADE / RESTRICT */
	else if (Matches3("DROP",
					  "COLLATION|CONVERSION|DOMAIN|EXTENSION|LANGUAGE|SCHEMA|SEQUENCE|SERVER|TABLE|TYPE|VIEW",
					  MatchAny) ||
			 (Matches4("DROP", "AGGREGATE|FUNCTION", MatchAny, MatchAny) &&
			  ends_with(prev_wd, ')')) ||
			 Matches4("DROP", "EVENT", "TRIGGER", MatchAny) ||
			 Matches5("DROP", "FOREIGN", "DATA", "WRAPPER", MatchAny) ||
			 Matches4("DROP", "FOREIGN", "TABLE", MatchAny) ||
			 Matches5("DROP", "TEXT", "SEARCH", "CONFIGURATION|DICTIONARY|PARSER|TEMPLATE", MatchAny))
		COMPLETE_WITH_LIST2("CASCADE", "RESTRICT");

	/* help completing some of the variants */
	else if (Matches3("DROP", "AGGREGATE|FUNCTION", MatchAny))
		COMPLETE_WITH_CONST("(");
	else if (Matches4("DROP", "AGGREGATE|FUNCTION", MatchAny, "("))
		COMPLETE_WITH_FUNCTION_ARG(prev2_wd);
	else if (Matches2("DROP", "FOREIGN"))
		COMPLETE_WITH_LIST2("DATA WRAPPER", "TABLE");

	/* DROP INDEX */
	else if (Matches2("DROP", "INDEX"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes,
								   " UNION SELECT 'CONCURRENTLY'");
	else if (Matches3("DROP", "INDEX", "CONCURRENTLY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes, NULL);
	else if (Matches3("DROP", "INDEX", MatchAny))
		COMPLETE_WITH_LIST2("CASCADE", "RESTRICT");
	else if (Matches4("DROP", "INDEX", "CONCURRENTLY", MatchAny))
		COMPLETE_WITH_LIST2("CASCADE", "RESTRICT");

	/* DROP MATERIALIZED VIEW */
	else if (Matches2("DROP", "MATERIALIZED"))
		COMPLETE_WITH_CONST("VIEW");
	else if (Matches3("DROP", "MATERIALIZED", "VIEW"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews, NULL);

	/* DROP OWNED BY */
	else if (Matches2("DROP", "OWNED"))
		COMPLETE_WITH_CONST("BY");
	else if (Matches3("DROP", "OWNED", "BY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);

	else if (Matches3("DROP", "TEXT", "SEARCH"))
		COMPLETE_WITH_LIST4("CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE");

	/* DROP TRIGGER */
	else if (Matches3("DROP", "TRIGGER", MatchAny))
		COMPLETE_WITH_CONST("ON");
	else if (Matches4("DROP", "TRIGGER", MatchAny, "ON"))
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_tables_for_trigger);
	}
	else if (Matches5("DROP", "TRIGGER", MatchAny, "ON", MatchAny))
		COMPLETE_WITH_LIST2("CASCADE", "RESTRICT");

	/* DROP EVENT TRIGGER */
	else if (Matches2("DROP", "EVENT"))
		COMPLETE_WITH_CONST("TRIGGER");
	else if (Matches3("DROP", "EVENT", "TRIGGER"))
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);

	/* DROP POLICY <name>  */
	else if (Matches2("DROP", "POLICY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_policies);
	/* DROP POLICY <name> ON */
	else if (Matches3("DROP", "POLICY", MatchAny))
		COMPLETE_WITH_CONST("ON");
	/* DROP POLICY <name> ON <table> */
	else if (Matches4("DROP", "POLICY", MatchAny, "ON"))
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_tables_for_policy);
	}

	/* DROP RULE */
	else if (Matches3("DROP", "RULE", MatchAny))
		COMPLETE_WITH_CONST("ON");
	else if (Matches4("DROP", "RULE", MatchAny, "ON"))
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_tables_for_rule);
	}
	else if (Matches5("DROP", "RULE", MatchAny, "ON", MatchAny))
		COMPLETE_WITH_LIST2("CASCADE", "RESTRICT");

/* EXECUTE */
	else if (Matches1("EXECUTE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_prepared_statements);

/* EXPLAIN */

	/*
	 * Complete EXPLAIN [ANALYZE] [VERBOSE] with list of EXPLAIN-able commands
	 */
	else if (Matches1("EXPLAIN"))
		COMPLETE_WITH_LIST7("SELECT", "INSERT", "DELETE", "UPDATE", "DECLARE",
							"ANALYZE", "VERBOSE");
	else if (Matches2("EXPLAIN", "ANALYZE"))
		COMPLETE_WITH_LIST6("SELECT", "INSERT", "DELETE", "UPDATE", "DECLARE",
							"VERBOSE");
	else if (Matches2("EXPLAIN", "VERBOSE") ||
			 Matches3("EXPLAIN", "ANALYZE", "VERBOSE"))
		COMPLETE_WITH_LIST5("SELECT", "INSERT", "DELETE", "UPDATE", "DECLARE");

/* FETCH && MOVE */
	/* Complete FETCH with one of FORWARD, BACKWARD, RELATIVE */
	else if (Matches1("FETCH|MOVE"))
		COMPLETE_WITH_LIST4("ABSOLUTE", "BACKWARD", "FORWARD", "RELATIVE");
	/* Complete FETCH <sth> with one of ALL, NEXT, PRIOR */
	else if (Matches2("FETCH|MOVE", MatchAny))
		COMPLETE_WITH_LIST3("ALL", "NEXT", "PRIOR");

	/*
	 * Complete FETCH <sth1> <sth2> with "FROM" or "IN". These are equivalent,
	 * but we may as well tab-complete both: perhaps some users prefer one
	 * variant or the other.
	 */
	else if (Matches3("FETCH|MOVE", MatchAny, MatchAny))
		COMPLETE_WITH_LIST2("FROM", "IN");

/* FOREIGN DATA WRAPPER */
	/* applies in ALTER/DROP FDW and in CREATE SERVER */
	else if (TailMatches3("FOREIGN", "DATA", "WRAPPER") &&
			 !TailMatches4("CREATE", MatchAny, MatchAny, MatchAny))
		COMPLETE_WITH_QUERY(Query_for_list_of_fdws);
	/* applies in CREATE SERVER */
	else if (TailMatches4("FOREIGN", "DATA", "WRAPPER", MatchAny) &&
			 HeadMatches2("CREATE", "SERVER"))
		COMPLETE_WITH_CONST("OPTIONS");

/* FOREIGN TABLE */
	else if (TailMatches2("FOREIGN", "TABLE") &&
			 !TailMatches3("CREATE", MatchAny, MatchAny))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_foreign_tables, NULL);

/* FOREIGN SERVER */
	else if (TailMatches2("FOREIGN", "SERVER"))
		COMPLETE_WITH_QUERY(Query_for_list_of_servers);

/* GRANT && REVOKE --- is allowed inside CREATE SCHEMA, so use TailMatches */
	/* Complete GRANT/REVOKE with a list of roles and privileges */
	else if (TailMatches1("GRANT|REVOKE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles
							" UNION SELECT 'SELECT'"
							" UNION SELECT 'INSERT'"
							" UNION SELECT 'UPDATE'"
							" UNION SELECT 'DELETE'"
							" UNION SELECT 'TRUNCATE'"
							" UNION SELECT 'REFERENCES'"
							" UNION SELECT 'TRIGGER'"
							" UNION SELECT 'CREATE'"
							" UNION SELECT 'CONNECT'"
							" UNION SELECT 'TEMPORARY'"
							" UNION SELECT 'EXECUTE'"
							" UNION SELECT 'USAGE'"
							" UNION SELECT 'ALL'");

	/*
	 * Complete GRANT/REVOKE <privilege> with "ON", GRANT/REVOKE <role> with
	 * TO/FROM
	 */
	else if (TailMatches2("GRANT|REVOKE", MatchAny))
	{
		if (TailMatches1("SELECT|INSERT|UPDATE|DELETE|TRUNCATE|REFERENCES|TRIGGER|CREATE|CONNECT|TEMPORARY|TEMP|EXECUTE|USAGE|ALL"))
			COMPLETE_WITH_CONST("ON");
		else if (TailMatches2("GRANT", MatchAny))
			COMPLETE_WITH_CONST("TO");
		else
			COMPLETE_WITH_CONST("FROM");
	}

	/*
	 * Complete GRANT/REVOKE <sth> ON with a list of tables, views, and
	 * sequences.
	 *
	 * Keywords like DATABASE, FUNCTION, LANGUAGE and SCHEMA added to query
	 * result via UNION; seems to work intuitively.
	 *
	 * Note: GRANT/REVOKE can get quite complex; tab-completion as implemented
	 * here will only work if the privilege list contains exactly one
	 * privilege.
	 */
	else if (TailMatches3("GRANT|REVOKE", MatchAny, "ON"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsvmf,
								   " UNION SELECT 'ALL FUNCTIONS IN SCHEMA'"
								   " UNION SELECT 'ALL SEQUENCES IN SCHEMA'"
								   " UNION SELECT 'ALL TABLES IN SCHEMA'"
								   " UNION SELECT 'DATABASE'"
								   " UNION SELECT 'DOMAIN'"
								   " UNION SELECT 'FOREIGN DATA WRAPPER'"
								   " UNION SELECT 'FOREIGN SERVER'"
								   " UNION SELECT 'FUNCTION'"
								   " UNION SELECT 'LANGUAGE'"
								   " UNION SELECT 'LARGE OBJECT'"
								   " UNION SELECT 'SCHEMA'"
								   " UNION SELECT 'SEQUENCE'"
								   " UNION SELECT 'TABLE'"
								   " UNION SELECT 'TABLESPACE'"
								   " UNION SELECT 'TYPE'");

	else if (TailMatches4("GRANT|REVOKE", MatchAny, "ON", "ALL"))
		COMPLETE_WITH_LIST3("FUNCTIONS IN SCHEMA", "SEQUENCES IN SCHEMA",
							"TABLES IN SCHEMA");

	else if (TailMatches4("GRANT|REVOKE", MatchAny, "ON", "FOREIGN"))
		COMPLETE_WITH_LIST2("DATA WRAPPER", "SERVER");

	/*
	 * Complete "GRANT/REVOKE * ON DATABASE/DOMAIN/..." with a list of
	 * appropriate objects.
	 *
	 * Complete "GRANT/REVOKE * ON *" with "TO/FROM".
	 */
	else if (TailMatches4("GRANT|REVOKE", MatchAny, "ON", MatchAny))
	{
		if (TailMatches1("DATABASE"))
			COMPLETE_WITH_QUERY(Query_for_list_of_databases);
		else if (TailMatches1("DOMAIN"))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_domains, NULL);
		else if (TailMatches1("FUNCTION"))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions, NULL);
		else if (TailMatches1("LANGUAGE"))
			COMPLETE_WITH_QUERY(Query_for_list_of_languages);
		else if (TailMatches1("SCHEMA"))
			COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
		else if (TailMatches1("SEQUENCE"))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_sequences, NULL);
		else if (TailMatches1("TABLE"))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsvmf, NULL);
		else if (TailMatches1("TABLESPACE"))
			COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
		else if (TailMatches1("TYPE"))
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes, NULL);
		else if (TailMatches4("GRANT", MatchAny, MatchAny, MatchAny))
			COMPLETE_WITH_CONST("TO");
		else
			COMPLETE_WITH_CONST("FROM");
	}

	/*
	 * Complete "GRANT/REVOKE ... TO/FROM" with username, PUBLIC,
	 * CURRENT_USER, or SESSION_USER.
	 */
	else if ((HeadMatches1("GRANT") && TailMatches1("TO")) ||
			 (HeadMatches1("REVOKE") && TailMatches1("FROM")))
		COMPLETE_WITH_QUERY(Query_for_list_of_grant_roles);

	/* Complete "GRANT/REVOKE ... ON * *" with TO/FROM */
	else if (HeadMatches1("GRANT") && TailMatches3("ON", MatchAny, MatchAny))
		COMPLETE_WITH_CONST("TO");
	else if (HeadMatches1("REVOKE") && TailMatches3("ON", MatchAny, MatchAny))
		COMPLETE_WITH_CONST("FROM");

	/* Complete "GRANT/REVOKE * ON ALL * IN SCHEMA *" with TO/FROM */
	else if (TailMatches8("GRANT|REVOKE", MatchAny, "ON", "ALL", MatchAny, "IN", "SCHEMA", MatchAny))
	{
		if (TailMatches8("GRANT", MatchAny, MatchAny, MatchAny, MatchAny, MatchAny, MatchAny, MatchAny))
			COMPLETE_WITH_CONST("TO");
		else
			COMPLETE_WITH_CONST("FROM");
	}

	/* Complete "GRANT/REVOKE * ON FOREIGN DATA WRAPPER *" with TO/FROM */
	else if (TailMatches7("GRANT|REVOKE", MatchAny, "ON", "FOREIGN", "DATA", "WRAPPER", MatchAny))
	{
		if (TailMatches7("GRANT", MatchAny, MatchAny, MatchAny, MatchAny, MatchAny, MatchAny))
			COMPLETE_WITH_CONST("TO");
		else
			COMPLETE_WITH_CONST("FROM");
	}

	/* Complete "GRANT/REVOKE * ON FOREIGN SERVER *" with TO/FROM */
	else if (TailMatches6("GRANT|REVOKE", MatchAny, "ON", "FOREIGN", "SERVER", MatchAny))
	{
		if (TailMatches6("GRANT", MatchAny, MatchAny, MatchAny, MatchAny, MatchAny))
			COMPLETE_WITH_CONST("TO");
		else
			COMPLETE_WITH_CONST("FROM");
	}

/* GROUP BY */
	else if (TailMatches3("FROM", MatchAny, "GROUP"))
		COMPLETE_WITH_CONST("BY");

/* IMPORT FOREIGN SCHEMA */
	else if (Matches1("IMPORT"))
		COMPLETE_WITH_CONST("FOREIGN SCHEMA");
	else if (Matches2("IMPORT", "FOREIGN"))
		COMPLETE_WITH_CONST("SCHEMA");

/* INSERT --- can be inside EXPLAIN, RULE, etc */
	/* Complete INSERT with "INTO" */
	else if (TailMatches1("INSERT"))
		COMPLETE_WITH_CONST("INTO");
	/* Complete INSERT INTO with table names */
	else if (TailMatches2("INSERT", "INTO"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_updatables, NULL);
	/* Complete "INSERT INTO <table> (" with attribute names */
	else if (TailMatches4("INSERT", "INTO", MatchAny, "("))
		COMPLETE_WITH_ATTR(prev2_wd, "");

	/*
	 * Complete INSERT INTO <table> with "(" or "VALUES" or "SELECT" or
	 * "TABLE" or "DEFAULT VALUES"
	 */
	else if (TailMatches3("INSERT", "INTO", MatchAny))
		COMPLETE_WITH_LIST5("(", "DEFAULT VALUES", "SELECT", "TABLE", "VALUES");

	/*
	 * Complete INSERT INTO <table> (attribs) with "VALUES" or "SELECT" or
	 * "TABLE"
	 */
	else if (TailMatches4("INSERT", "INTO", MatchAny, MatchAny) &&
			 ends_with(prev_wd, ')'))
		COMPLETE_WITH_LIST3("SELECT", "TABLE", "VALUES");

	/* Insert an open parenthesis after "VALUES" */
	else if (TailMatches1("VALUES") && !TailMatches2("DEFAULT", "VALUES"))
		COMPLETE_WITH_CONST("(");

/* LOCK */
	/* Complete LOCK [TABLE] with a list of tables */
	else if (Matches1("LOCK"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   " UNION SELECT 'TABLE'");
	else if (Matches2("LOCK", "TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, "");

	/* For the following, handle the case of a single table only for now */

	/* Complete LOCK [TABLE] <table> with "IN" */
	else if (Matches2("LOCK", MatchAnyExcept("TABLE")) ||
			 Matches3("LOCK", "TABLE", MatchAny))
		COMPLETE_WITH_CONST("IN");

	/* Complete LOCK [TABLE] <table> IN with a lock mode */
	else if (Matches3("LOCK", MatchAny, "IN") ||
			 Matches4("LOCK", "TABLE", MatchAny, "IN"))
		COMPLETE_WITH_LIST8("ACCESS SHARE MODE",
							"ROW SHARE MODE", "ROW EXCLUSIVE MODE",
							"SHARE UPDATE EXCLUSIVE MODE", "SHARE MODE",
							"SHARE ROW EXCLUSIVE MODE",
							"EXCLUSIVE MODE", "ACCESS EXCLUSIVE MODE");

/* NOTIFY --- can be inside EXPLAIN, RULE, etc */
	else if (TailMatches1("NOTIFY"))
		COMPLETE_WITH_QUERY("SELECT pg_catalog.quote_ident(channel) FROM pg_catalog.pg_listening_channels() AS channel WHERE substring(pg_catalog.quote_ident(channel),1,%d)='%s'");

/* OPTIONS */
	else if (TailMatches1("OPTIONS"))
		COMPLETE_WITH_CONST("(");

/* OWNER TO  - complete with available roles */
	else if (TailMatches2("OWNER", "TO"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);

/* ORDER BY */
	else if (TailMatches3("FROM", MatchAny, "ORDER"))
		COMPLETE_WITH_CONST("BY");
	else if (TailMatches4("FROM", MatchAny, "ORDER", "BY"))
		COMPLETE_WITH_ATTR(prev3_wd, "");

/* PREPARE xx AS */
	else if (Matches3("PREPARE", MatchAny, "AS"))
		COMPLETE_WITH_LIST4("SELECT", "UPDATE", "INSERT", "DELETE FROM");

/*
 * PREPARE TRANSACTION is missing on purpose. It's intended for transaction
 * managers, not for manual use in interactive sessions.
 */

/* REASSIGN OWNED BY xxx TO yyy */
	else if (Matches1("REASSIGN"))
		COMPLETE_WITH_CONST("OWNED BY");
	else if (Matches2("REASSIGN", "OWNED"))
		COMPLETE_WITH_CONST("BY");
	else if (Matches3("REASSIGN", "OWNED", "BY"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (Matches4("REASSIGN", "OWNED", "BY", MatchAny))
		COMPLETE_WITH_CONST("TO");
	else if (Matches5("REASSIGN", "OWNED", "BY", MatchAny, "TO"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);

/* REFRESH MATERIALIZED VIEW */
	else if (Matches1("REFRESH"))
		COMPLETE_WITH_CONST("MATERIALIZED VIEW");
	else if (Matches2("REFRESH", "MATERIALIZED"))
		COMPLETE_WITH_CONST("VIEW");
	else if (Matches3("REFRESH", "MATERIALIZED", "VIEW"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews,
								   " UNION SELECT 'CONCURRENTLY'");
	else if (Matches4("REFRESH", "MATERIALIZED", "VIEW", "CONCURRENTLY"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews, NULL);
	else if (Matches4("REFRESH", "MATERIALIZED", "VIEW", MatchAny))
		COMPLETE_WITH_CONST("WITH");
	else if (Matches5("REFRESH", "MATERIALIZED", "VIEW", "CONCURRENTLY", MatchAny))
		COMPLETE_WITH_CONST("WITH");
	else if (Matches5("REFRESH", "MATERIALIZED", "VIEW", MatchAny, "WITH"))
		COMPLETE_WITH_LIST2("NO DATA", "DATA");
	else if (Matches6("REFRESH", "MATERIALIZED", "VIEW", "CONCURRENTLY", MatchAny, "WITH"))
		COMPLETE_WITH_LIST2("NO DATA", "DATA");
	else if (Matches6("REFRESH", "MATERIALIZED", "VIEW", MatchAny, "WITH", "NO"))
		COMPLETE_WITH_CONST("DATA");
	else if (Matches7("REFRESH", "MATERIALIZED", "VIEW", "CONCURRENTLY", MatchAny, "WITH", "NO"))
		COMPLETE_WITH_CONST("DATA");

/* REINDEX */
	else if (Matches1("REINDEX"))
		COMPLETE_WITH_LIST5("TABLE", "INDEX", "SYSTEM", "SCHEMA", "DATABASE");
	else if (Matches2("REINDEX", "TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm, NULL);
	else if (Matches2("REINDEX", "INDEX"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes, NULL);
	else if (Matches2("REINDEX", "SCHEMA"))
		COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
	else if (Matches2("REINDEX", "SYSTEM|DATABASE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_databases);

/* SECURITY LABEL */
	else if (Matches1("SECURITY"))
		COMPLETE_WITH_CONST("LABEL");
	else if (Matches2("SECURITY", "LABEL"))
		COMPLETE_WITH_LIST2("ON", "FOR");
	else if (Matches4("SECURITY", "LABEL", "FOR", MatchAny))
		COMPLETE_WITH_CONST("ON");
	else if (Matches3("SECURITY", "LABEL", "ON") ||
			 Matches5("SECURITY", "LABEL", "FOR", MatchAny, "ON"))
	{
		static const char *const list_SECURITY_LABEL[] =
		{"TABLE", "COLUMN", "AGGREGATE", "DATABASE", "DOMAIN",
			"EVENT TRIGGER", "FOREIGN TABLE", "FUNCTION", "LARGE OBJECT",
			"MATERIALIZED VIEW", "LANGUAGE", "ROLE", "SCHEMA",
		"SEQUENCE", "TABLESPACE", "TYPE", "VIEW", NULL};

		COMPLETE_WITH_LIST(list_SECURITY_LABEL);
	}
	else if (Matches5("SECURITY", "LABEL", "ON", MatchAny, MatchAny))
		COMPLETE_WITH_CONST("IS");

/* SELECT */
	/* naah . . . */

/* SET, RESET, SHOW */
	/* Complete with a variable name */
	else if (TailMatches1("SET|RESET") && !TailMatches3("UPDATE", MatchAny, "SET"))
		COMPLETE_WITH_QUERY(Query_for_list_of_set_vars);
	else if (Matches1("SHOW"))
		COMPLETE_WITH_QUERY(Query_for_list_of_show_vars);
	/* Complete "SET TRANSACTION" */
	else if (Matches2("SET|BEGIN|START", "TRANSACTION") ||
			 Matches2("BEGIN", "WORK") ||
		  Matches5("SET", "SESSION", "CHARACTERISTICS", "AS", "TRANSACTION"))
		COMPLETE_WITH_LIST2("ISOLATION LEVEL", "READ");
	else if (Matches3("SET|BEGIN|START", "TRANSACTION|WORK", "ISOLATION") ||
			 Matches6("SET", "SESSION", "CHARACTERISTICS", "AS", "TRANSACTION", "ISOLATION"))
		COMPLETE_WITH_CONST("LEVEL");
	else if (Matches4("SET|BEGIN|START", "TRANSACTION|WORK", "ISOLATION", "LEVEL"))
		COMPLETE_WITH_LIST3("READ", "REPEATABLE READ", "SERIALIZABLE");
	else if (Matches5("SET|BEGIN|START", "TRANSACTION|WORK", "ISOLATION", "LEVEL", "READ"))
		COMPLETE_WITH_LIST2("UNCOMMITTED", "COMMITTED");
	else if (Matches5("SET|BEGIN|START", "TRANSACTION|WORK", "ISOLATION", "LEVEL", "REPEATABLE"))
		COMPLETE_WITH_CONST("READ");
	else if (Matches3("SET|BEGIN|START", "TRANSACTION|WORK", "READ"))
		COMPLETE_WITH_LIST2("ONLY", "WRITE");
	/* SET CONSTRAINTS */
	else if (Matches2("SET", "CONSTRAINTS"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_constraints_with_schema, "UNION SELECT 'ALL'");
	/* Complete SET CONSTRAINTS <foo> with DEFERRED|IMMEDIATE */
	else if (Matches3("SET", "CONSTRAINTS", MatchAny))
		COMPLETE_WITH_LIST2("DEFERRED", "IMMEDIATE");
	/* Complete SET ROLE */
	else if (Matches2("SET", "ROLE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	/* Complete SET SESSION with AUTHORIZATION or CHARACTERISTICS... */
	else if (Matches2("SET", "SESSION"))
		COMPLETE_WITH_LIST2("AUTHORIZATION", "CHARACTERISTICS AS TRANSACTION");
	/* Complete SET SESSION AUTHORIZATION with username */
	else if (Matches3("SET", "SESSION", "AUTHORIZATION"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles " UNION SELECT 'DEFAULT'");
	/* Complete RESET SESSION with AUTHORIZATION */
	else if (Matches2("RESET", "SESSION"))
		COMPLETE_WITH_CONST("AUTHORIZATION");
	/* Complete SET <var> with "TO" */
	else if (Matches2("SET", MatchAny))
		COMPLETE_WITH_CONST("TO");
	/* Complete ALTER DATABASE|FUNCTION|ROLE|USER ... SET <name> */
	else if (HeadMatches2("ALTER", "DATABASE|FUNCTION|ROLE|USER") &&
			 TailMatches2("SET", MatchAny))
		COMPLETE_WITH_LIST2("FROM CURRENT", "TO");
	/* Suggest possible variable values */
	else if (TailMatches3("SET", MatchAny, "TO|="))
	{
		/* special cased code for individual GUCs */
		if (TailMatches2("DateStyle", "TO|="))
		{
			static const char *const my_list[] =
			{"ISO", "SQL", "Postgres", "German",
				"YMD", "DMY", "MDY",
				"US", "European", "NonEuropean",
			"DEFAULT", NULL};

			COMPLETE_WITH_LIST(my_list);
		}
		else if (TailMatches2("search_path", "TO|="))
			COMPLETE_WITH_QUERY(Query_for_list_of_schemas
								" AND nspname not like 'pg\\_toast%%' "
								" AND nspname not like 'pg\\_temp%%' "
								" UNION SELECT 'DEFAULT' ");
		else
		{
			/* generic, type based, GUC support */
			char	   *guctype = get_guctype(prev2_wd);

			if (guctype && strcmp(guctype, "enum") == 0)
			{
				char		querybuf[1024];

				snprintf(querybuf, sizeof(querybuf), Query_for_enum, prev2_wd);
				COMPLETE_WITH_QUERY(querybuf);
			}
			else if (guctype && strcmp(guctype, "bool") == 0)
				COMPLETE_WITH_LIST9("on", "off", "true", "false", "yes", "no",
									"1", "0", "DEFAULT");
			else
				COMPLETE_WITH_CONST("DEFAULT");

			if (guctype)
				free(guctype);
		}
	}

/* START TRANSACTION */
	else if (Matches1("START"))
		COMPLETE_WITH_CONST("TRANSACTION");

/* TABLE, but not TABLE embedded in other commands */
	else if (Matches1("TABLE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_relations, NULL);

/* TABLESAMPLE */
	else if (TailMatches1("TABLESAMPLE"))
		COMPLETE_WITH_QUERY(Query_for_list_of_tablesample_methods);
	else if (TailMatches2("TABLESAMPLE", MatchAny))
		COMPLETE_WITH_CONST("(");

/* TRUNCATE */
	else if (Matches1("TRUNCATE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);

/* UNLISTEN */
	else if (Matches1("UNLISTEN"))
		COMPLETE_WITH_QUERY("SELECT pg_catalog.quote_ident(channel) FROM pg_catalog.pg_listening_channels() AS channel WHERE substring(pg_catalog.quote_ident(channel),1,%d)='%s' UNION SELECT '*'");

/* UPDATE --- can be inside EXPLAIN, RULE, etc */
	/* If prev. word is UPDATE suggest a list of tables */
	else if (TailMatches1("UPDATE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_updatables, NULL);
	/* Complete UPDATE <table> with "SET" */
	else if (TailMatches2("UPDATE", MatchAny))
		COMPLETE_WITH_CONST("SET");
	/* Complete UPDATE <table> SET with list of attributes */
	else if (TailMatches3("UPDATE", MatchAny, "SET"))
		COMPLETE_WITH_ATTR(prev2_wd, "");
	/* UPDATE <table> SET <attr> = */
	else if (TailMatches4("UPDATE", MatchAny, "SET", MatchAny))
		COMPLETE_WITH_CONST("=");

/* USER MAPPING */
	else if (Matches3("ALTER|CREATE|DROP", "USER", "MAPPING"))
		COMPLETE_WITH_CONST("FOR");
	else if (Matches4("CREATE", "USER", "MAPPING", "FOR"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles
							" UNION SELECT 'CURRENT_USER'"
							" UNION SELECT 'PUBLIC'"
							" UNION SELECT 'USER'");
	else if (Matches4("ALTER|DROP", "USER", "MAPPING", "FOR"))
		COMPLETE_WITH_QUERY(Query_for_list_of_user_mappings);
	else if (Matches5("CREATE|ALTER|DROP", "USER", "MAPPING", "FOR", MatchAny))
		COMPLETE_WITH_CONST("SERVER");
	else if (Matches7("CREATE|ALTER", "USER", "MAPPING", "FOR", MatchAny, "SERVER", MatchAny))
		COMPLETE_WITH_CONST("OPTIONS");

/*
 * VACUUM [ FULL | FREEZE ] [ VERBOSE ] [ table ]
 * VACUUM [ FULL | FREEZE ] [ VERBOSE ] ANALYZE [ table [ (column [, ...] ) ] ]
 */
	else if (Matches1("VACUUM"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'FULL'"
								   " UNION SELECT 'FREEZE'"
								   " UNION SELECT 'ANALYZE'"
								   " UNION SELECT 'VERBOSE'");
	else if (Matches2("VACUUM", "FULL|FREEZE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'ANALYZE'"
								   " UNION SELECT 'VERBOSE'");
	else if (Matches3("VACUUM", "FULL|FREEZE", "ANALYZE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'VERBOSE'");
	else if (Matches3("VACUUM", "FULL|FREEZE", "VERBOSE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'ANALYZE'");
	else if (Matches2("VACUUM", "VERBOSE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'ANALYZE'");
	else if (Matches2("VACUUM", "ANALYZE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'VERBOSE'");
	else if (HeadMatches1("VACUUM"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm, NULL);

/* WITH [RECURSIVE] */

	/*
	 * Only match when WITH is the first word, as WITH may appear in many
	 * other contexts.
	 */
	else if (Matches1("WITH"))
		COMPLETE_WITH_CONST("RECURSIVE");

/* ANALYZE */
	/* Complete with list of tables */
	else if (Matches1("ANALYZE"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tmf, NULL);

/* WHERE */
	/* Simple case of the word before the where being the table name */
	else if (TailMatches2(MatchAny, "WHERE"))
		COMPLETE_WITH_ATTR(prev2_wd, "");

/* ... FROM ... */
/* TODO: also include SRF ? */
	else if (TailMatches1("FROM") && !Matches3("COPY|\\copy", MatchAny, "FROM"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsvmf, NULL);

/* ... JOIN ... */
	else if (TailMatches1("JOIN"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsvmf, NULL);

/* Backslash commands */
/* TODO:  \dc \dd \dl */
	else if (TailMatchesCS1("\\?"))
		COMPLETE_WITH_LIST_CS3("commands", "options", "variables");
	else if (TailMatchesCS1("\\connect|\\c"))
	{
		if (!recognized_connection_string(text))
			COMPLETE_WITH_QUERY(Query_for_list_of_databases);
	}
	else if (TailMatchesCS2("\\connect|\\c", MatchAny))
	{
		if (!recognized_connection_string(prev_wd))
			COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	}
	else if (TailMatchesCS1("\\da*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_aggregates, NULL);
	else if (TailMatchesCS1("\\db*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
	else if (TailMatchesCS1("\\dD*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_domains, NULL);
	else if (TailMatchesCS1("\\des*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_servers);
	else if (TailMatchesCS1("\\deu*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_user_mappings);
	else if (TailMatchesCS1("\\dew*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_fdws);
	else if (TailMatchesCS1("\\df*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions, NULL);

	else if (TailMatchesCS1("\\dFd*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_dictionaries);
	else if (TailMatchesCS1("\\dFp*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_parsers);
	else if (TailMatchesCS1("\\dFt*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_templates);
	/* must be at end of \dF alternatives: */
	else if (TailMatchesCS1("\\dF*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_configurations);

	else if (TailMatchesCS1("\\di*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes, NULL);
	else if (TailMatchesCS1("\\dL*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_languages);
	else if (TailMatchesCS1("\\dn*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
	else if (TailMatchesCS1("\\dp") || TailMatchesCS1("\\z"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsvmf, NULL);
	else if (TailMatchesCS1("\\ds*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_sequences, NULL);
	else if (TailMatchesCS1("\\dt*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
	else if (TailMatchesCS1("\\dT*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes, NULL);
	else if (TailMatchesCS1("\\du*") || TailMatchesCS1("\\dg*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (TailMatchesCS1("\\dv*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views, NULL);
	else if (TailMatchesCS1("\\dx*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_extensions);
	else if (TailMatchesCS1("\\dm*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews, NULL);
	else if (TailMatchesCS1("\\dE*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_foreign_tables, NULL);
	else if (TailMatchesCS1("\\dy*"))
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);

	/* must be at end of \d alternatives: */
	else if (TailMatchesCS1("\\d*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_relations, NULL);

	else if (TailMatchesCS1("\\ef"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions, NULL);
	else if (TailMatchesCS1("\\ev"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views, NULL);

	else if (TailMatchesCS1("\\encoding"))
		COMPLETE_WITH_QUERY(Query_for_list_of_encodings);
	else if (TailMatchesCS1("\\h") || TailMatchesCS1("\\help"))
		COMPLETE_WITH_LIST(sql_commands);
	else if (TailMatchesCS1("\\password"))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (TailMatchesCS1("\\pset"))
	{
		static const char *const my_list[] =
		{"border", "columns", "expanded", "fieldsep", "fieldsep_zero",
			"footer", "format", "linestyle", "null", "numericlocale",
			"pager", "recordsep", "recordsep_zero", "tableattr", "title",
			"tuples_only", "unicode_border_linestyle",
		"unicode_column_linestyle", "unicode_header_linestyle", NULL};

		COMPLETE_WITH_LIST_CS(my_list);
	}
	else if (TailMatchesCS2("\\pset", MatchAny))
	{
		if (TailMatchesCS1("format"))
		{
			static const char *const my_list[] =
			{"unaligned", "aligned", "wrapped", "html", "asciidoc",
			"latex", "latex-longtable", "troff-ms", NULL};

			COMPLETE_WITH_LIST_CS(my_list);
		}
		else if (TailMatchesCS1("linestyle"))
			COMPLETE_WITH_LIST_CS3("ascii", "old-ascii", "unicode");
		else if (TailMatchesCS1("unicode_border_linestyle|"
								"unicode_column_linestyle|"
								"unicode_header_linestyle"))
			COMPLETE_WITH_LIST_CS2("single", "double");
	}
	else if (TailMatchesCS1("\\unset"))
	{
		matches = complete_from_variables(text, "", "", true);
	}
	else if (TailMatchesCS1("\\set"))
	{
		matches = complete_from_variables(text, "", "", false);
	}
	else if (TailMatchesCS2("\\set", MatchAny))
	{
		if (TailMatchesCS1("AUTOCOMMIT|ON_ERROR_STOP|QUIET|"
						   "SINGLELINE|SINGLESTEP"))
			COMPLETE_WITH_LIST_CS2("on", "off");
		else if (TailMatchesCS1("COMP_KEYWORD_CASE"))
			COMPLETE_WITH_LIST_CS4("lower", "upper",
								   "preserve-lower", "preserve-upper");
		else if (TailMatchesCS1("ECHO"))
			COMPLETE_WITH_LIST_CS4("errors", "queries", "all", "none");
		else if (TailMatchesCS1("ECHO_HIDDEN"))
			COMPLETE_WITH_LIST_CS3("noexec", "off", "on");
		else if (TailMatchesCS1("HISTCONTROL"))
			COMPLETE_WITH_LIST_CS4("ignorespace", "ignoredups",
								   "ignoreboth", "none");
		else if (TailMatchesCS1("ON_ERROR_ROLLBACK"))
			COMPLETE_WITH_LIST_CS3("on", "off", "interactive");
		else if (TailMatchesCS1("SHOW_CONTEXT"))
			COMPLETE_WITH_LIST_CS3("never", "errors", "always");
		else if (TailMatchesCS1("VERBOSITY"))
			COMPLETE_WITH_LIST_CS3("default", "verbose", "terse");
	}
	else if (TailMatchesCS1("\\sf*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions, NULL);
	else if (TailMatchesCS1("\\sv*"))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views, NULL);
	else if (TailMatchesCS1("\\cd|\\e|\\edit|\\g|\\i|\\include|"
							"\\ir|\\include_relative|\\o|\\out|"
							"\\s|\\w|\\write|\\lo_import"))
	{
		completion_charp = "\\";
		matches = completion_matches(text, complete_from_files);
	}

	/*
	 * Finally, we look through the list of "things", such as TABLE, INDEX and
	 * check if that was the previous word. If so, execute the query to get a
	 * list of them.
	 */
	else
	{
		int			i;

		for (i = 0; words_after_create[i].name; i++)
		{
			if (pg_strcasecmp(prev_wd, words_after_create[i].name) == 0)
			{
				if (words_after_create[i].query)
					COMPLETE_WITH_QUERY(words_after_create[i].query);
				else if (words_after_create[i].squery)
					COMPLETE_WITH_SCHEMA_QUERY(*words_after_create[i].squery,
											   NULL);
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
		COMPLETE_WITH_CONST("");
#ifdef HAVE_RL_COMPLETION_APPEND_CHARACTER
		rl_completion_append_character = '\0';
#endif
	}

	/* free storage */
	free(previous_words);
	free(words_buffer);

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

/* The following two functions are wrappers for _complete_from_query */

static char *
complete_from_query(const char *text, int state)
{
	return _complete_from_query(0, text, state);
}

static char *
complete_from_schema_query(const char *text, int state)
{
	return _complete_from_query(1, text, state);
}


/*
 * This creates a list of matching things, according to a query pointed to
 * by completion_charp.
 * The query can be one of two kinds:
 *
 * 1. A simple query which must contain a %d and a %s, which will be replaced
 * by the string length of the text and the text itself. The query may also
 * have up to four more %s in it; the first two such will be replaced by the
 * value of completion_info_charp, the next two by the value of
 * completion_info_charp2.
 *
 * 2. A schema query used for completion of both schema and relation names.
 * These are more complex and must contain in the following order:
 * %d %s %d %s %d %s %s %d %s
 * where %d is the string length of the text and %s the text itself.
 *
 * It is assumed that strings should be escaped to become SQL literals
 * (that is, what is in the query is actually ... '%s' ...)
 *
 * See top of file for examples of both kinds of query.
 */
static char *
_complete_from_query(int is_schema_query, const char *text, int state)
{
	static int	list_index,
				string_length;
	static PGresult *result = NULL;

	/*
	 * If this is the first time for this completion, we fetch a list of our
	 * "things" from the backend.
	 */
	if (state == 0)
	{
		PQExpBufferData query_buffer;
		char	   *e_text;
		char	   *e_info_charp;
		char	   *e_info_charp2;

		list_index = 0;
		string_length = strlen(text);

		/* Free any prior result */
		PQclear(result);
		result = NULL;

		/* Set up suitably-escaped copies of textual inputs */
		e_text = escape_string(text);

		if (completion_info_charp)
			e_info_charp = escape_string(completion_info_charp);
		else
			e_info_charp = NULL;

		if (completion_info_charp2)
			e_info_charp2 = escape_string(completion_info_charp2);
		else
			e_info_charp2 = NULL;

		initPQExpBuffer(&query_buffer);

		if (is_schema_query)
		{
			/* completion_squery gives us the pieces to assemble */
			const char *qualresult = completion_squery->qualresult;

			if (qualresult == NULL)
				qualresult = completion_squery->result;

			/* Get unqualified names matching the input-so-far */
			appendPQExpBuffer(&query_buffer, "SELECT %s FROM %s WHERE ",
							  completion_squery->result,
							  completion_squery->catname);
			if (completion_squery->selcondition)
				appendPQExpBuffer(&query_buffer, "%s AND ",
								  completion_squery->selcondition);
			appendPQExpBuffer(&query_buffer, "substring(%s,1,%d)='%s'",
							  completion_squery->result,
							  string_length, e_text);
			appendPQExpBuffer(&query_buffer, " AND %s",
							  completion_squery->viscondition);

			/*
			 * When fetching relation names, suppress system catalogs unless
			 * the input-so-far begins with "pg_".  This is a compromise
			 * between not offering system catalogs for completion at all, and
			 * having them swamp the result when the input is just "p".
			 */
			if (strcmp(completion_squery->catname,
					   "pg_catalog.pg_class c") == 0 &&
				strncmp(text, "pg_", 3) !=0)
			{
				appendPQExpBufferStr(&query_buffer,
									 " AND c.relnamespace <> (SELECT oid FROM"
				   " pg_catalog.pg_namespace WHERE nspname = 'pg_catalog')");
			}

			/*
			 * Add in matching schema names, but only if there is more than
			 * one potential match among schema names.
			 */
			appendPQExpBuffer(&query_buffer, "\nUNION\n"
						   "SELECT pg_catalog.quote_ident(n.nspname) || '.' "
							  "FROM pg_catalog.pg_namespace n "
							  "WHERE substring(pg_catalog.quote_ident(n.nspname) || '.',1,%d)='%s'",
							  string_length, e_text);
			appendPQExpBuffer(&query_buffer,
							  " AND (SELECT pg_catalog.count(*)"
							  " FROM pg_catalog.pg_namespace"
			" WHERE substring(pg_catalog.quote_ident(nspname) || '.',1,%d) ="
							  " substring('%s',1,pg_catalog.length(pg_catalog.quote_ident(nspname))+1)) > 1",
							  string_length, e_text);

			/*
			 * Add in matching qualified names, but only if there is exactly
			 * one schema matching the input-so-far.
			 */
			appendPQExpBuffer(&query_buffer, "\nUNION\n"
					 "SELECT pg_catalog.quote_ident(n.nspname) || '.' || %s "
							  "FROM %s, pg_catalog.pg_namespace n "
							  "WHERE %s = n.oid AND ",
							  qualresult,
							  completion_squery->catname,
							  completion_squery->namespace);
			if (completion_squery->selcondition)
				appendPQExpBuffer(&query_buffer, "%s AND ",
								  completion_squery->selcondition);
			appendPQExpBuffer(&query_buffer, "substring(pg_catalog.quote_ident(n.nspname) || '.' || %s,1,%d)='%s'",
							  qualresult,
							  string_length, e_text);

			/*
			 * This condition exploits the single-matching-schema rule to
			 * speed up the query
			 */
			appendPQExpBuffer(&query_buffer,
			" AND substring(pg_catalog.quote_ident(n.nspname) || '.',1,%d) ="
							  " substring('%s',1,pg_catalog.length(pg_catalog.quote_ident(n.nspname))+1)",
							  string_length, e_text);
			appendPQExpBuffer(&query_buffer,
							  " AND (SELECT pg_catalog.count(*)"
							  " FROM pg_catalog.pg_namespace"
			" WHERE substring(pg_catalog.quote_ident(nspname) || '.',1,%d) ="
							  " substring('%s',1,pg_catalog.length(pg_catalog.quote_ident(nspname))+1)) = 1",
							  string_length, e_text);

			/* If an addon query was provided, use it */
			if (completion_charp)
				appendPQExpBuffer(&query_buffer, "\n%s", completion_charp);
		}
		else
		{
			/* completion_charp is an sprintf-style format string */
			appendPQExpBuffer(&query_buffer, completion_charp,
							  string_length, e_text,
							  e_info_charp, e_info_charp,
							  e_info_charp2, e_info_charp2);
		}

		/* Limit the number of records in the result */
		appendPQExpBuffer(&query_buffer, "\nLIMIT %d",
						  completion_max_records);

		result = exec_query(query_buffer.data);

		termPQExpBuffer(&query_buffer);
		free(e_text);
		if (e_info_charp)
			free(e_info_charp);
		if (e_info_charp2)
			free(e_info_charp2);
	}

	/* Find something that matches */
	if (result && PQresultStatus(result) == PGRES_TUPLES_OK)
	{
		const char *item;

		while (list_index < PQntuples(result) &&
			   (item = PQgetvalue(result, list_index++, 0)))
			if (pg_strncasecmp(text, item, string_length) == 0)
				return pg_strdup(item);
	}

	/* If nothing matches, free the db structure and return null */
	PQclear(result);
	result = NULL;
	return NULL;
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
 * match what's there, and nothing the second time. This should be used if
 * there is only one possibility that can appear at a certain spot, so
 * misspellings will be overwritten.  The string to be passed must be in
 * completion_charp.
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
 * to support quoting usages. If need_value is true, only the variables
 * that have the set values are picked up.
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

	static const char *const known_varnames[] = {
		"AUTOCOMMIT", "COMP_KEYWORD_CASE", "DBNAME", "ECHO", "ECHO_HIDDEN",
		"ENCODING", "FETCH_COUNT", "HISTCONTROL", "HISTFILE", "HISTSIZE",
		"HOST", "IGNOREEOF", "LASTOID", "ON_ERROR_ROLLBACK", "ON_ERROR_STOP",
		"PORT", "PROMPT1", "PROMPT2", "PROMPT3", "QUIET",
		"SHOW_CONTEXT", "SINGLELINE", "SINGLESTEP",
		"USER", "VERBOSITY", NULL
	};

	varnames = (char **) pg_malloc((maxvars + 1) * sizeof(char *));

	if (!need_value)
	{
		for (i = 0; known_varnames[i] && nvars < maxvars; i++)
			append_variable_names(&varnames, &nvars, &maxvars,
								  known_varnames[i], prefix, suffix);
	}

	for (ptr = pset.vars->next; ptr; ptr = ptr->next)
	{
		if (need_value && !(ptr->value))
			continue;
		for (i = 0; known_varnames[i]; i++)		/* remove duplicate entry */
		{
			if (strcmp(ptr->name, known_varnames[i]) == 0)
				continue;
		}
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
 */
static char *
complete_from_files(const char *text, int state)
{
	static const char *unquoted_text;
	char	   *unquoted_match;
	char	   *ret = NULL;

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

	unquoted_match = filename_completion_function(unquoted_text, state);
	if (unquoted_match)
	{
		/*
		 * Caller sets completion_charp to a zero- or one-character string
		 * containing the escape character.  This is necessary since \copy has
		 * no escape character, but every other backslash command recognizes
		 * "\" as an escape character.  Since we have only two callers, don't
		 * bother providing a macro to simplify this.
		 */
		ret = quote_if_needed(unquoted_match, " \t\r\n\"`",
							  '\'', *completion_charp, pset.encoding);
		if (ret)
			free(unquoted_match);
		else
			ret = unquoted_match;
	}

	return ret;
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
 * Execute a query and report any errors. This should be the preferred way of
 * talking to the database in this file.
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
#ifdef NOT_USED
		psql_error("tab completion query failed: %s\nQuery was:\n%s\n",
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

#ifdef NOT_USED

/*
 * Surround a string with single quotes. This works for both SQL and
 * psql internal. Currently disabled because it is reported not to
 * cooperate with certain versions of readline.
 */
static char *
quote_file_name(char *text, int match_type, char *quote_pointer)
{
	char	   *s;
	size_t		length;

	(void) quote_pointer;		/* not used */

	length = strlen(text) +(match_type == SINGLE_MATCH ? 3 : 2);
	s = pg_malloc(length);
	s[0] = '\'';
	strcpy(s + 1, text);
	if (match_type == SINGLE_MATCH)
		s[length - 2] = '\'';
	s[length - 1] = '\0';
	return s;
}

static char *
dequote_file_name(char *text, char quote_char)
{
	char	   *s;
	size_t		length;

	if (!quote_char)
		return pg_strdup(text);

	length = strlen(text);
	s = pg_malloc(length - 2 + 1);
	strlcpy(s, text +1, length - 2 + 1);

	return s;
}
#endif   /* NOT_USED */

#endif   /* USE_READLINE */
