/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2014, PostgreSQL Global Development Group
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
 * See `man 3 readline' or `info readline' for the full details. Also,
 * hence the
 *
 * BUGS:
 *
 * - If you split your queries across lines, this whole thing gets
 *	 confused. (To fix this, one would have to read psql's query
 *	 buffer rather than readline's line buffer, which would require
 *	 some major revisions of things.)
 *
 * - Table or attribute names with spaces in it may confuse it.
 *
 * - Quotes, parenthesis, and other funny characters are not handled
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
 * 3) The items from a null-pointer-terminated list.
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
" UNION ALL SELECT 'PUBLIC'"

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
#define Query_for_list_of_tables_for_rule \
"SELECT pg_catalog.quote_ident(relname) "\
"  FROM pg_catalog.pg_class"\
" WHERE (%d = pg_catalog.length('%s'))"\
"   AND oid IN "\
"       (SELECT ev_class FROM pg_catalog.pg_rewrite "\
"         WHERE pg_catalog.quote_ident(rulename)='%s')"

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

#define Query_for_list_of_prepared_statements \
" SELECT pg_catalog.quote_ident(name) "\
"   FROM pg_catalog.pg_prepared_statements "\
"  WHERE substring(pg_catalog.quote_ident(name),1,%d)='%s'"

#define Query_for_list_of_event_triggers \
" SELECT pg_catalog.quote_ident(evtname) "\
"   FROM pg_catalog.pg_event_trigger "\
"  WHERE substring(pg_catalog.quote_ident(evtname),1,%d)='%s'"

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
	{"MATERIALIZED VIEW", NULL, NULL},
	{"OPERATOR", NULL, NULL},	/* Querying for this is probably not such a
								 * good idea. */
	{"OWNED", NULL, NULL, THING_NO_CREATE},		/* for DROP OWNED BY ... */
	{"PARSER", Query_for_list_of_ts_parsers, NULL, THING_NO_SHOW},
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
	{"TRIGGER", "SELECT pg_catalog.quote_ident(tgname) FROM pg_catalog.pg_trigger WHERE substring(pg_catalog.quote_ident(tgname),1,%d)='%s'"},
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
static char **complete_from_variables(const char *text,
						const char *prefix, const char *suffix);
static char *complete_from_files(const char *text, int state);

static char *pg_strdup_keyword_case(const char *s, const char *ref);
static PGresult *exec_query(const char *query);

static void get_previous_words(int point, char **previous_words, int nwords);

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

	/* This array will contain some scannage of the input line. */
	char	   *previous_words[6];

	/* For compactness, we use these macros to reference previous_words[]. */
#define prev_wd   (previous_words[0])
#define prev2_wd  (previous_words[1])
#define prev3_wd  (previous_words[2])
#define prev4_wd  (previous_words[3])
#define prev5_wd  (previous_words[4])
#define prev6_wd  (previous_words[5])

	static const char *const sql_commands[] = {
		"ABORT", "ALTER", "ANALYZE", "BEGIN", "CHECKPOINT", "CLOSE", "CLUSTER",
		"COMMENT", "COMMIT", "COPY", "CREATE", "DEALLOCATE", "DECLARE",
		"DELETE FROM", "DISCARD", "DO", "DROP", "END", "EXECUTE", "EXPLAIN", "FETCH",
		"GRANT", "INSERT", "LISTEN", "LOAD", "LOCK", "MOVE", "NOTIFY", "PREPARE",
		"REASSIGN", "REFRESH", "REINDEX", "RELEASE", "RESET", "REVOKE", "ROLLBACK",
		"SAVEPOINT", "SECURITY LABEL", "SELECT", "SET", "SHOW", "START",
		"TABLE", "TRUNCATE", "UNLISTEN", "UPDATE", "VACUUM", "VALUES", "WITH",
		NULL
	};

	static const char *const backslash_commands[] = {
		"\\a", "\\connect", "\\conninfo", "\\C", "\\cd", "\\copy", "\\copyright",
		"\\d", "\\da", "\\db", "\\dc", "\\dC", "\\dd", "\\dD", "\\des", "\\det", "\\deu", "\\dew", "\\df",
		"\\dF", "\\dFd", "\\dFp", "\\dFt", "\\dg", "\\di", "\\dl", "\\dL",
		"\\dn", "\\do", "\\dp", "\\drds", "\\ds", "\\dS", "\\dt", "\\dT", "\\dv", "\\du", "\\dx",
		"\\e", "\\echo", "\\ef", "\\encoding",
		"\\f", "\\g", "\\gset", "\\h", "\\help", "\\H", "\\i", "\\ir", "\\l",
		"\\lo_import", "\\lo_export", "\\lo_list", "\\lo_unlink",
		"\\o", "\\p", "\\password", "\\prompt", "\\pset", "\\q", "\\qecho", "\\r",
		"\\set", "\\sf", "\\t", "\\T",
		"\\timing", "\\unset", "\\x", "\\w", "\\watch", "\\z", "\\!", NULL
	};

	(void) end;					/* not used */

#ifdef HAVE_RL_COMPLETION_APPEND_CHARACTER
	rl_completion_append_character = ' ';
#endif

	/* Clear a few things. */
	completion_charp = NULL;
	completion_charpp = NULL;
	completion_info_charp = NULL;
	completion_info_charp2 = NULL;

	/*
	 * Scan the input line before our current position for the last few words.
	 * According to those we'll make some smart decisions on what the user is
	 * probably intending to type.
	 */
	get_previous_words(start, previous_words, lengthof(previous_words));

	/* If a backslash command was started, continue */
	if (text[0] == '\\')
		COMPLETE_WITH_LIST_CS(backslash_commands);

	/* Variable interpolation */
	else if (text[0] == ':' && text[1] != ':')
	{
		if (text[1] == '\'')
			matches = complete_from_variables(text, ":'", "'");
		else if (text[1] == '"')
			matches = complete_from_variables(text, ":\"", "\"");
		else
			matches = complete_from_variables(text, ":", "");
	}

	/* If no previous word, suggest one of the basic sql commands */
	else if (prev_wd[0] == '\0')
		COMPLETE_WITH_LIST(sql_commands);

/* CREATE */
	/* complete with something you can create */
	else if (pg_strcasecmp(prev_wd, "CREATE") == 0)
		matches = completion_matches(text, create_command_generator);

/* DROP, but not DROP embedded in other commands */
	/* complete with something you can drop */
	else if (pg_strcasecmp(prev_wd, "DROP") == 0 &&
			 prev2_wd[0] == '\0')
		matches = completion_matches(text, drop_command_generator);

/* ALTER */

	/* ALTER TABLE */
	else if (pg_strcasecmp(prev2_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev_wd, "TABLE") == 0)
	{
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   "UNION SELECT 'ALL IN TABLESPACE'");
	}
	/*
	 * complete with what you can alter (TABLE, GROUP, USER, ...) unless we're
	 * in ALTER TABLE sth ALTER
	 */
	else if (pg_strcasecmp(prev_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TABLE") != 0)
	{
		static const char *const list_ALTER[] =
		{"AGGREGATE", "COLLATION", "CONVERSION", "DATABASE", "DEFAULT PRIVILEGES", "DOMAIN",
			"EVENT TRIGGER", "EXTENSION", "FOREIGN DATA WRAPPER", "FOREIGN TABLE", "FUNCTION",
			"GROUP", "INDEX", "LANGUAGE", "LARGE OBJECT", "MATERIALIZED VIEW", "OPERATOR",
			"ROLE", "RULE", "SCHEMA", "SERVER", "SEQUENCE", "SYSTEM", "TABLE",
			"TABLESPACE", "TEXT SEARCH", "TRIGGER", "TYPE",
		"USER", "USER MAPPING FOR", "VIEW", NULL};

		COMPLETE_WITH_LIST(list_ALTER);
	}
	/* ALTER TABLE,INDEX,MATERIALIZED VIEW xxx ALL IN TABLESPACE xxx */
	else if (pg_strcasecmp(prev4_wd, "ALL") == 0 &&
			 pg_strcasecmp(prev3_wd, "IN") == 0 &&
			 pg_strcasecmp(prev2_wd, "TABLESPACE") == 0)
	{
		static const char *const list_ALTERALLINTSPC[] =
			{"SET TABLESPACE", "OWNED BY", NULL};

		COMPLETE_WITH_LIST(list_ALTERALLINTSPC);
	}
	/* ALTER TABLE,INDEX,MATERIALIZED VIEW xxx ALL IN TABLESPACE xxx OWNED BY */
	else if (pg_strcasecmp(prev6_wd, "ALL") == 0 &&
			 pg_strcasecmp(prev5_wd, "IN") == 0 &&
			 pg_strcasecmp(prev4_wd, "TABLESPACE") == 0 &&
			 pg_strcasecmp(prev2_wd, "OWNED") == 0 &&
			 pg_strcasecmp(prev4_wd, "BY") == 0)
	{
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	}
	/* ALTER AGGREGATE,FUNCTION <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 (pg_strcasecmp(prev2_wd, "AGGREGATE") == 0 ||
			  pg_strcasecmp(prev2_wd, "FUNCTION") == 0))
		COMPLETE_WITH_CONST("(");
	/* ALTER AGGREGATE,FUNCTION <name> (...) */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 (pg_strcasecmp(prev3_wd, "AGGREGATE") == 0 ||
			  pg_strcasecmp(prev3_wd, "FUNCTION") == 0))
	{
		if (prev_wd[strlen(prev_wd) - 1] == ')')
		{
			static const char *const list_ALTERAGG[] =
			{"OWNER TO", "RENAME TO", "SET SCHEMA", NULL};

			COMPLETE_WITH_LIST(list_ALTERAGG);
		}
		else
			COMPLETE_WITH_FUNCTION_ARG(prev2_wd);
	}

	/* ALTER SCHEMA <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "SCHEMA") == 0)
	{
		static const char *const list_ALTERGEN[] =
		{"OWNER TO", "RENAME TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERGEN);
	}

	/* ALTER COLLATION <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "COLLATION") == 0)
	{
		static const char *const list_ALTERGEN[] =
		{"OWNER TO", "RENAME TO", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERGEN);
	}

	/* ALTER CONVERSION <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "CONVERSION") == 0)
	{
		static const char *const list_ALTERGEN[] =
		{"OWNER TO", "RENAME TO", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERGEN);
	}

	/* ALTER DATABASE <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "DATABASE") == 0)
	{
		static const char *const list_ALTERDATABASE[] =
		{"RESET", "SET", "OWNER TO", "RENAME TO", "CONNECTION LIMIT", NULL};

		COMPLETE_WITH_LIST(list_ALTERDATABASE);
	}

	/* ALTER EVENT TRIGGER */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "EVENT") == 0 &&
			 pg_strcasecmp(prev_wd, "TRIGGER") == 0)
	{
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);
	}

	/* ALTER EVENT TRIGGER <name> */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "EVENT") == 0 &&
			 pg_strcasecmp(prev2_wd, "TRIGGER") == 0)
	{
		static const char *const list_ALTER_EVENT_TRIGGER[] =
		{"DISABLE", "ENABLE", "OWNER TO", "RENAME TO", NULL};

		COMPLETE_WITH_LIST(list_ALTER_EVENT_TRIGGER);
	}

	/* ALTER EVENT TRIGGER <name> ENABLE */
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "EVENT") == 0 &&
			 pg_strcasecmp(prev3_wd, "TRIGGER") == 0 &&
			 pg_strcasecmp(prev_wd, "ENABLE") == 0)
	{
		static const char *const list_ALTER_EVENT_TRIGGER_ENABLE[] =
		{"REPLICA", "ALWAYS", NULL};

		COMPLETE_WITH_LIST(list_ALTER_EVENT_TRIGGER_ENABLE);
	}

	/* ALTER EXTENSION <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "EXTENSION") == 0)
	{
		static const char *const list_ALTEREXTENSION[] =
		{"ADD", "DROP", "UPDATE", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTEREXTENSION);
	}

	/* ALTER FOREIGN */
	else if (pg_strcasecmp(prev2_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev_wd, "FOREIGN") == 0)
	{
		static const char *const list_ALTER_FOREIGN[] =
		{"DATA WRAPPER", "TABLE", NULL};

		COMPLETE_WITH_LIST(list_ALTER_FOREIGN);
	}

	/* ALTER FOREIGN DATA WRAPPER <name> */
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "FOREIGN") == 0 &&
			 pg_strcasecmp(prev3_wd, "DATA") == 0 &&
			 pg_strcasecmp(prev2_wd, "WRAPPER") == 0)
	{
		static const char *const list_ALTER_FDW[] =
		{"HANDLER", "VALIDATOR", "OPTIONS", "OWNER TO", NULL};

		COMPLETE_WITH_LIST(list_ALTER_FDW);
	}

	/* ALTER FOREIGN TABLE <name> */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "FOREIGN") == 0 &&
			 pg_strcasecmp(prev2_wd, "TABLE") == 0)
	{
		static const char *const list_ALTER_FOREIGN_TABLE[] =
		{"ALTER", "DROP", "RENAME", "OWNER TO", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTER_FOREIGN_TABLE);
	}

	/* ALTER INDEX */
	else if (pg_strcasecmp(prev2_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev_wd, "INDEX") == 0)
	{
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes,
								   "UNION SELECT 'ALL IN TABLESPACE'");
	}
	/* ALTER INDEX <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "INDEX") == 0)
	{
		static const char *const list_ALTERINDEX[] =
		{"OWNER TO", "RENAME TO", "SET", "RESET", NULL};

		COMPLETE_WITH_LIST(list_ALTERINDEX);
	}
	/* ALTER INDEX <name> SET */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "INDEX") == 0 &&
			 pg_strcasecmp(prev_wd, "SET") == 0)
	{
		static const char *const list_ALTERINDEXSET[] =
		{"(", "TABLESPACE", NULL};

		COMPLETE_WITH_LIST(list_ALTERINDEXSET);
	}
	/* ALTER INDEX <name> RESET */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "INDEX") == 0 &&
			 pg_strcasecmp(prev_wd, "RESET") == 0)
		COMPLETE_WITH_CONST("(");
	/* ALTER INDEX <foo> SET|RESET ( */
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "INDEX") == 0 &&
			 (pg_strcasecmp(prev2_wd, "SET") == 0 ||
			  pg_strcasecmp(prev2_wd, "RESET") == 0) &&
			 pg_strcasecmp(prev_wd, "(") == 0)
	{
		static const char *const list_INDEXOPTIONS[] =
		{"fillfactor", "fastupdate", NULL};

		COMPLETE_WITH_LIST(list_INDEXOPTIONS);
	}

	/* ALTER LANGUAGE <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "LANGUAGE") == 0)
	{
		static const char *const list_ALTERLANGUAGE[] =
		{"OWNER TO", "RENAME TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERLANGUAGE);
	}

	/* ALTER LARGE OBJECT <oid> */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "LARGE") == 0 &&
			 pg_strcasecmp(prev2_wd, "OBJECT") == 0)
	{
		static const char *const list_ALTERLARGEOBJECT[] =
		{"OWNER TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERLARGEOBJECT);
	}

	/* ALTER MATERIALIZED VIEW */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev_wd, "VIEW") == 0)
	{
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews,
								   "UNION SELECT 'ALL IN TABLESPACE'");
	}

	/* ALTER USER,ROLE <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 !(pg_strcasecmp(prev2_wd, "USER") == 0 && pg_strcasecmp(prev_wd, "MAPPING") == 0) &&
			 (pg_strcasecmp(prev2_wd, "USER") == 0 ||
			  pg_strcasecmp(prev2_wd, "ROLE") == 0))
	{
		static const char *const list_ALTERUSER[] =
		{"CONNECTION LIMIT", "CREATEDB", "CREATEROLE", "CREATEUSER",
			"ENCRYPTED", "INHERIT", "LOGIN", "NOCREATEDB", "NOCREATEROLE",
			"NOCREATEUSER", "NOINHERIT", "NOLOGIN", "NOREPLICATION",
			"NOSUPERUSER", "RENAME TO", "REPLICATION", "RESET", "SET",
		"SUPERUSER", "UNENCRYPTED", "VALID UNTIL", "WITH", NULL};

		COMPLETE_WITH_LIST(list_ALTERUSER);
	}

	/* ALTER USER,ROLE <name> WITH */
	else if ((pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			  (pg_strcasecmp(prev3_wd, "USER") == 0 ||
			   pg_strcasecmp(prev3_wd, "ROLE") == 0) &&
			  pg_strcasecmp(prev_wd, "WITH") == 0))
	{
		/* Similar to the above, but don't complete "WITH" again. */
		static const char *const list_ALTERUSER_WITH[] =
		{"CONNECTION LIMIT", "CREATEDB", "CREATEROLE", "CREATEUSER",
			"ENCRYPTED", "INHERIT", "LOGIN", "NOCREATEDB", "NOCREATEROLE",
			"NOCREATEUSER", "NOINHERIT", "NOLOGIN", "NOREPLICATION",
			"NOSUPERUSER", "RENAME TO", "REPLICATION", "RESET", "SET",
		"SUPERUSER", "UNENCRYPTED", "VALID UNTIL", NULL};

		COMPLETE_WITH_LIST(list_ALTERUSER_WITH);
	}

	/* complete ALTER USER,ROLE <name> ENCRYPTED,UNENCRYPTED with PASSWORD */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 (pg_strcasecmp(prev3_wd, "ROLE") == 0 || pg_strcasecmp(prev3_wd, "USER") == 0) &&
			 (pg_strcasecmp(prev_wd, "ENCRYPTED") == 0 || pg_strcasecmp(prev_wd, "UNENCRYPTED") == 0))
	{
		COMPLETE_WITH_CONST("PASSWORD");
	}
	/* ALTER DEFAULT PRIVILEGES */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "DEFAULT") == 0 &&
			 pg_strcasecmp(prev_wd, "PRIVILEGES") == 0)
	{
		static const char *const list_ALTER_DEFAULT_PRIVILEGES[] =
		{"FOR ROLE", "FOR USER", "IN SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTER_DEFAULT_PRIVILEGES);
	}
	/* ALTER DEFAULT PRIVILEGES FOR */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "DEFAULT") == 0 &&
			 pg_strcasecmp(prev2_wd, "PRIVILEGES") == 0 &&
			 pg_strcasecmp(prev_wd, "FOR") == 0)
	{
		static const char *const list_ALTER_DEFAULT_PRIVILEGES_FOR[] =
		{"ROLE", "USER", NULL};

		COMPLETE_WITH_LIST(list_ALTER_DEFAULT_PRIVILEGES_FOR);
	}
	/* ALTER DEFAULT PRIVILEGES { FOR ROLE ... | IN SCHEMA ... } */
	else if (pg_strcasecmp(prev5_wd, "DEFAULT") == 0 &&
			 pg_strcasecmp(prev4_wd, "PRIVILEGES") == 0 &&
			 (pg_strcasecmp(prev3_wd, "FOR") == 0 ||
			  pg_strcasecmp(prev3_wd, "IN") == 0))
	{
		static const char *const list_ALTER_DEFAULT_PRIVILEGES_REST[] =
		{"GRANT", "REVOKE", NULL};

		COMPLETE_WITH_LIST(list_ALTER_DEFAULT_PRIVILEGES_REST);
	}
	/* ALTER DOMAIN <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "DOMAIN") == 0)
	{
		static const char *const list_ALTERDOMAIN[] =
		{"ADD", "DROP", "OWNER TO", "RENAME", "SET", "VALIDATE CONSTRAINT", NULL};

		COMPLETE_WITH_LIST(list_ALTERDOMAIN);
	}
	/* ALTER DOMAIN <sth> DROP */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "DOMAIN") == 0 &&
			 pg_strcasecmp(prev_wd, "DROP") == 0)
	{
		static const char *const list_ALTERDOMAIN2[] =
		{"CONSTRAINT", "DEFAULT", "NOT NULL", NULL};

		COMPLETE_WITH_LIST(list_ALTERDOMAIN2);
	}
	/* ALTER DOMAIN <sth> DROP|RENAME|VALIDATE CONSTRAINT */
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "DOMAIN") == 0 &&
			 (pg_strcasecmp(prev2_wd, "DROP") == 0 ||
			  pg_strcasecmp(prev2_wd, "RENAME") == 0 ||
			  pg_strcasecmp(prev2_wd, "VALIDATE") == 0) &&
			 pg_strcasecmp(prev_wd, "CONSTRAINT") == 0)
	{
		completion_info_charp = prev3_wd;
		COMPLETE_WITH_QUERY(Query_for_constraint_of_type);
	}
	/* ALTER DOMAIN <sth> RENAME */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "DOMAIN") == 0 &&
			 pg_strcasecmp(prev_wd, "RENAME") == 0)
	{
		static const char *const list_ALTERDOMAIN[] =
		{"CONSTRAINT", "TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERDOMAIN);
	}
	/* ALTER DOMAIN <sth> RENAME CONSTRAINT <sth> */
	else if (pg_strcasecmp(prev5_wd, "DOMAIN") == 0 &&
			 pg_strcasecmp(prev3_wd, "RENAME") == 0 &&
			 pg_strcasecmp(prev2_wd, "CONSTRAINT") == 0)
		COMPLETE_WITH_CONST("TO");

	/* ALTER DOMAIN <sth> SET */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "DOMAIN") == 0 &&
			 pg_strcasecmp(prev_wd, "SET") == 0)
	{
		static const char *const list_ALTERDOMAIN3[] =
		{"DEFAULT", "NOT NULL", "SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERDOMAIN3);
	}
	/* ALTER SEQUENCE <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "SEQUENCE") == 0)
	{
		static const char *const list_ALTERSEQUENCE[] =
		{"INCREMENT", "MINVALUE", "MAXVALUE", "RESTART", "NO", "CACHE", "CYCLE",
		"SET SCHEMA", "OWNED BY", "OWNER TO", "RENAME TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERSEQUENCE);
	}
	/* ALTER SEQUENCE <name> NO */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "SEQUENCE") == 0 &&
			 pg_strcasecmp(prev_wd, "NO") == 0)
	{
		static const char *const list_ALTERSEQUENCE2[] =
		{"MINVALUE", "MAXVALUE", "CYCLE", NULL};

		COMPLETE_WITH_LIST(list_ALTERSEQUENCE2);
	}
	/* ALTER SERVER <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "SERVER") == 0)
	{
		static const char *const list_ALTER_SERVER[] =
		{"VERSION", "OPTIONS", "OWNER TO", NULL};

		COMPLETE_WITH_LIST(list_ALTER_SERVER);
	}
	/* ALTER SYSTEM SET, RESET, RESET ALL */
	else if (pg_strcasecmp(prev2_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev_wd, "SYSTEM") == 0)
	{
		static const char *const list_ALTERSYSTEM[] =
		{"SET", "RESET", NULL};

		COMPLETE_WITH_LIST(list_ALTERSYSTEM);
	}
	/* ALTER SYSTEM SET|RESET <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "SYSTEM") == 0 &&
			 (pg_strcasecmp(prev_wd, "SET") == 0 ||
			 pg_strcasecmp(prev_wd, "RESET") == 0))
		COMPLETE_WITH_QUERY(Query_for_list_of_alter_system_set_vars);
	/* ALTER VIEW <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "VIEW") == 0)
	{
		static const char *const list_ALTERVIEW[] =
		{"ALTER COLUMN", "OWNER TO", "RENAME TO", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERVIEW);
	}
	/* ALTER MATERIALIZED VIEW <name> */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev2_wd, "VIEW") == 0)
	{
		static const char *const list_ALTERMATVIEW[] =
		{"ALTER COLUMN", "OWNER TO", "RENAME TO", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERMATVIEW);
	}

	/* ALTER RULE <name>, add ON */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "RULE") == 0)
		COMPLETE_WITH_CONST("ON");

	/* If we have ALTER RULE <name> ON, then add the correct tablename */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "RULE") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_tables_for_rule);
	}

	/* ALTER RULE <name> ON <name> */
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "RULE") == 0)
		COMPLETE_WITH_CONST("RENAME TO");

	/* ALTER TRIGGER <name>, add ON */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "TRIGGER") == 0)
		COMPLETE_WITH_CONST("ON");

	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TRIGGER") == 0)
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_tables_for_trigger);
	}

	/*
	 * If we have ALTER TRIGGER <sth> ON, then add the correct tablename
	 */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TRIGGER") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);

	/* ALTER TRIGGER <name> ON <name> */
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "TRIGGER") == 0 &&
			 pg_strcasecmp(prev2_wd, "ON") == 0)
		COMPLETE_WITH_CONST("RENAME TO");

	/*
	 * If we detect ALTER TABLE <name>, suggest sub commands
	 */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "TABLE") == 0)
	{
		static const char *const list_ALTER2[] =
		{"ADD", "ALTER", "CLUSTER ON", "DISABLE", "DROP", "ENABLE", "INHERIT",
			"NO INHERIT", "RENAME", "RESET", "OWNER TO", "SET",
		"VALIDATE CONSTRAINT", "REPLICA IDENTITY", NULL};

		COMPLETE_WITH_LIST(list_ALTER2);
	}
	/* ALTER TABLE xxx ENABLE */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "ENABLE") == 0)
	{
		static const char *const list_ALTERENABLE[] =
		{"ALWAYS", "REPLICA", "RULE", "TRIGGER", NULL};

		COMPLETE_WITH_LIST(list_ALTERENABLE);
	}
	else if (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev2_wd, "ENABLE") == 0 &&
			 (pg_strcasecmp(prev_wd, "REPLICA") == 0 ||
			  pg_strcasecmp(prev_wd, "ALWAYS") == 0))
	{
		static const char *const list_ALTERENABLE2[] =
		{"RULE", "TRIGGER", NULL};

		COMPLETE_WITH_LIST(list_ALTERENABLE2);
	}
	/* ALTER TABLE xxx INHERIT */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "INHERIT") == 0)
	{
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, "");
	}
	/* ALTER TABLE xxx NO INHERIT */
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev2_wd, "NO") == 0 &&
			 pg_strcasecmp(prev_wd, "INHERIT") == 0)
	{
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, "");
	}
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "DISABLE") == 0)
	{
		static const char *const list_ALTERDISABLE[] =
		{"RULE", "TRIGGER", NULL};

		COMPLETE_WITH_LIST(list_ALTERDISABLE);
	}

	/* ALTER TABLE xxx ALTER */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "ALTER") == 0)
		COMPLETE_WITH_ATTR(prev2_wd, " UNION SELECT 'COLUMN' UNION SELECT 'CONSTRAINT'");

	/* ALTER TABLE xxx RENAME */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "RENAME") == 0)
		COMPLETE_WITH_ATTR(prev2_wd, " UNION SELECT 'COLUMN' UNION SELECT 'CONSTRAINT' UNION SELECT 'TO'");

	/*
	 * If we have TABLE <sth> ALTER COLUMN|RENAME COLUMN, provide list of
	 * columns
	 */
	else if (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 (pg_strcasecmp(prev2_wd, "ALTER") == 0 ||
			  pg_strcasecmp(prev2_wd, "RENAME") == 0) &&
			 pg_strcasecmp(prev_wd, "COLUMN") == 0)
		COMPLETE_WITH_ATTR(prev3_wd, "");

	/* ALTER TABLE xxx RENAME yyy */
	else if (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev2_wd, "RENAME") == 0 &&
			 pg_strcasecmp(prev_wd, "CONSTRAINT") != 0 &&
			 pg_strcasecmp(prev_wd, "TO") != 0)
		COMPLETE_WITH_CONST("TO");

	/* ALTER TABLE xxx RENAME COLUMN/CONSTRAINT yyy */
	else if (pg_strcasecmp(prev5_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev3_wd, "RENAME") == 0 &&
			 (pg_strcasecmp(prev2_wd, "COLUMN") == 0 ||
			  pg_strcasecmp(prev2_wd, "CONSTRAINT") == 0) &&
			 pg_strcasecmp(prev_wd, "TO") != 0)
		COMPLETE_WITH_CONST("TO");

	/* If we have TABLE <sth> DROP, provide COLUMN or CONSTRAINT */
	else if (pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "DROP") == 0)
	{
		static const char *const list_TABLEDROP[] =
		{"COLUMN", "CONSTRAINT", NULL};

		COMPLETE_WITH_LIST(list_TABLEDROP);
	}
	/* If we have ALTER TABLE <sth> DROP COLUMN, provide list of columns */
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev2_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev_wd, "COLUMN") == 0)
		COMPLETE_WITH_ATTR(prev3_wd, "");

	/*
	 * If we have ALTER TABLE <sth> ALTER|DROP|RENAME|VALIDATE CONSTRAINT,
	 * provide list of constraints
	 */
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 (pg_strcasecmp(prev2_wd, "ALTER") == 0 ||
			  pg_strcasecmp(prev2_wd, "DROP") == 0 ||
			  pg_strcasecmp(prev2_wd, "RENAME") == 0 ||
			  pg_strcasecmp(prev2_wd, "VALIDATE") == 0) &&
			 pg_strcasecmp(prev_wd, "CONSTRAINT") == 0)
	{
		completion_info_charp = prev3_wd;
		COMPLETE_WITH_QUERY(Query_for_constraint_of_table);
	}
	/* ALTER TABLE ALTER [COLUMN] <foo> */
	else if ((pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			  pg_strcasecmp(prev2_wd, "COLUMN") == 0) ||
			 (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			  pg_strcasecmp(prev2_wd, "ALTER") == 0))
	{
		static const char *const list_COLUMNALTER[] =
		{"TYPE", "SET", "RESET", "DROP", NULL};

		COMPLETE_WITH_LIST(list_COLUMNALTER);
	}
	/* ALTER TABLE ALTER [COLUMN] <foo> SET */
	else if (((pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			   pg_strcasecmp(prev3_wd, "COLUMN") == 0) ||
			  (pg_strcasecmp(prev5_wd, "TABLE") == 0 &&
			   pg_strcasecmp(prev3_wd, "ALTER") == 0)) &&
			 pg_strcasecmp(prev_wd, "SET") == 0)
	{
		static const char *const list_COLUMNSET[] =
		{"(", "DEFAULT", "NOT NULL", "STATISTICS", "STORAGE", NULL};

		COMPLETE_WITH_LIST(list_COLUMNSET);
	}
	/* ALTER TABLE ALTER [COLUMN] <foo> SET ( */
	else if (((pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			   pg_strcasecmp(prev4_wd, "COLUMN") == 0) ||
			  pg_strcasecmp(prev4_wd, "ALTER") == 0) &&
			 pg_strcasecmp(prev2_wd, "SET") == 0 &&
			 pg_strcasecmp(prev_wd, "(") == 0)
	{
		static const char *const list_COLUMNOPTIONS[] =
		{"n_distinct", "n_distinct_inherited", NULL};

		COMPLETE_WITH_LIST(list_COLUMNOPTIONS);
	}
	/* ALTER TABLE ALTER [COLUMN] <foo> SET STORAGE */
	else if (((pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			   pg_strcasecmp(prev4_wd, "COLUMN") == 0) ||
			  pg_strcasecmp(prev4_wd, "ALTER") == 0) &&
			 pg_strcasecmp(prev2_wd, "SET") == 0 &&
			 pg_strcasecmp(prev_wd, "STORAGE") == 0)
	{
		static const char *const list_COLUMNSTORAGE[] =
		{"PLAIN", "EXTERNAL", "EXTENDED", "MAIN", NULL};

		COMPLETE_WITH_LIST(list_COLUMNSTORAGE);
	}
	/* ALTER TABLE ALTER [COLUMN] <foo> DROP */
	else if (((pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			   pg_strcasecmp(prev3_wd, "COLUMN") == 0) ||
			  (pg_strcasecmp(prev5_wd, "TABLE") == 0 &&
			   pg_strcasecmp(prev3_wd, "ALTER") == 0)) &&
			 pg_strcasecmp(prev_wd, "DROP") == 0)
	{
		static const char *const list_COLUMNDROP[] =
		{"DEFAULT", "NOT NULL", NULL};

		COMPLETE_WITH_LIST(list_COLUMNDROP);
	}
	else if (pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "CLUSTER") == 0)
		COMPLETE_WITH_CONST("ON");
	else if (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev2_wd, "CLUSTER") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
	{
		completion_info_charp = prev3_wd;
		COMPLETE_WITH_QUERY(Query_for_index_of_table);
	}
	/* If we have TABLE <sth> SET, provide WITHOUT,TABLESPACE and SCHEMA */
	else if (pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "SET") == 0)
	{
		static const char *const list_TABLESET[] =
		{"(", "WITHOUT", "TABLESPACE", "SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_TABLESET);
	}
	/* If we have TABLE <sth> SET TABLESPACE provide a list of tablespaces */
	else if (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev2_wd, "SET") == 0 &&
			 pg_strcasecmp(prev_wd, "TABLESPACE") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
	/* If we have TABLE <sth> SET WITHOUT provide CLUSTER or OIDS */
	else if (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev2_wd, "SET") == 0 &&
			 pg_strcasecmp(prev_wd, "WITHOUT") == 0)
	{
		static const char *const list_TABLESET2[] =
		{"CLUSTER", "OIDS", NULL};

		COMPLETE_WITH_LIST(list_TABLESET2);
	}
	/* ALTER TABLE <foo> RESET */
	else if (pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "RESET") == 0)
		COMPLETE_WITH_CONST("(");
	/* ALTER TABLE <foo> SET|RESET ( */
	else if (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 (pg_strcasecmp(prev2_wd, "SET") == 0 ||
			  pg_strcasecmp(prev2_wd, "RESET") == 0) &&
			 pg_strcasecmp(prev_wd, "(") == 0)
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
			"user_catalog_table",
			NULL
		};

		COMPLETE_WITH_LIST(list_TABLEOPTIONS);
	}
	else if (pg_strcasecmp(prev4_wd, "REPLICA") == 0 &&
			 pg_strcasecmp(prev3_wd, "IDENTITY") == 0 &&
			 pg_strcasecmp(prev2_wd, "USING") == 0 &&
			 pg_strcasecmp(prev_wd, "INDEX") == 0)
	{
		completion_info_charp = prev5_wd;
		COMPLETE_WITH_QUERY(Query_for_index_of_table);
	}
	else if (pg_strcasecmp(prev5_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev3_wd, "REPLICA") == 0 &&
			 pg_strcasecmp(prev2_wd, "IDENTITY") == 0 &&
			 pg_strcasecmp(prev_wd, "USING") == 0)
	{
		COMPLETE_WITH_CONST("INDEX");
	}
	else if (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev2_wd, "REPLICA") == 0 &&
			 pg_strcasecmp(prev_wd, "IDENTITY") == 0)
	{
		static const char *const list_REPLICAID[] =
		{"FULL", "NOTHING", "DEFAULT", "USING", NULL};

		COMPLETE_WITH_LIST(list_REPLICAID);
	}
	else if (pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "REPLICA") == 0)
	{
		COMPLETE_WITH_CONST("IDENTITY");
	}

	/* ALTER TABLESPACE <foo> with RENAME TO, OWNER TO, SET, RESET */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "TABLESPACE") == 0)
	{
		static const char *const list_ALTERTSPC[] =
		{"RENAME TO", "OWNER TO", "SET", "RESET", NULL};

		COMPLETE_WITH_LIST(list_ALTERTSPC);
	}
	/* ALTER TABLESPACE <foo> SET|RESET */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TABLESPACE") == 0 &&
			 (pg_strcasecmp(prev_wd, "SET") == 0 ||
			  pg_strcasecmp(prev_wd, "RESET") == 0))
		COMPLETE_WITH_CONST("(");
	/* ALTER TABLESPACE <foo> SET|RESET ( */
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "TABLESPACE") == 0 &&
			 (pg_strcasecmp(prev2_wd, "SET") == 0 ||
			  pg_strcasecmp(prev2_wd, "RESET") == 0) &&
			 pg_strcasecmp(prev_wd, "(") == 0)
	{
		static const char *const list_TABLESPACEOPTIONS[] =
		{"seq_page_cost", "random_page_cost", NULL};

		COMPLETE_WITH_LIST(list_TABLESPACEOPTIONS);
	}

	/* ALTER TEXT SEARCH */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "TEXT") == 0 &&
			 pg_strcasecmp(prev_wd, "SEARCH") == 0)
	{
		static const char *const list_ALTERTEXTSEARCH[] =
		{"CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE", NULL};

		COMPLETE_WITH_LIST(list_ALTERTEXTSEARCH);
	}
	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "TEXT") == 0 &&
			 pg_strcasecmp(prev3_wd, "SEARCH") == 0 &&
			 (pg_strcasecmp(prev2_wd, "TEMPLATE") == 0 ||
			  pg_strcasecmp(prev2_wd, "PARSER") == 0))
	{
		static const char *const list_ALTERTEXTSEARCH2[] =
		{"RENAME TO", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERTEXTSEARCH2);
	}

	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "TEXT") == 0 &&
			 pg_strcasecmp(prev3_wd, "SEARCH") == 0 &&
			 pg_strcasecmp(prev2_wd, "DICTIONARY") == 0)
	{
		static const char *const list_ALTERTEXTSEARCH3[] =
		{"OWNER TO", "RENAME TO", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERTEXTSEARCH3);
	}

	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "TEXT") == 0 &&
			 pg_strcasecmp(prev3_wd, "SEARCH") == 0 &&
			 pg_strcasecmp(prev2_wd, "CONFIGURATION") == 0)
	{
		static const char *const list_ALTERTEXTSEARCH4[] =
		{"ADD MAPPING FOR", "ALTER MAPPING", "DROP MAPPING FOR", "OWNER TO", "RENAME TO", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERTEXTSEARCH4);
	}

	/* complete ALTER TYPE <foo> with actions */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "TYPE") == 0)
	{
		static const char *const list_ALTERTYPE[] =
		{"ADD ATTRIBUTE", "ADD VALUE", "ALTER ATTRIBUTE", "DROP ATTRIBUTE",
		"OWNER TO", "RENAME", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERTYPE);
	}
	/* complete ALTER TYPE <foo> ADD with actions */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TYPE") == 0 &&
			 pg_strcasecmp(prev_wd, "ADD") == 0)
	{
		static const char *const list_ALTERTYPE[] =
		{"ATTRIBUTE", "VALUE", NULL};

		COMPLETE_WITH_LIST(list_ALTERTYPE);
	}
	/* ALTER TYPE <foo> RENAME	*/
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TYPE") == 0 &&
			 pg_strcasecmp(prev_wd, "RENAME") == 0)
	{
		static const char *const list_ALTERTYPE[] =
		{"ATTRIBUTE", "TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERTYPE);
	}
	/* ALTER TYPE xxx RENAME ATTRIBUTE yyy */
	else if (pg_strcasecmp(prev5_wd, "TYPE") == 0 &&
			 pg_strcasecmp(prev3_wd, "RENAME") == 0 &&
			 pg_strcasecmp(prev2_wd, "ATTRIBUTE") == 0)
		COMPLETE_WITH_CONST("TO");

	/*
	 * If we have TYPE <sth> ALTER/DROP/RENAME ATTRIBUTE, provide list of
	 * attributes
	 */
	else if (pg_strcasecmp(prev4_wd, "TYPE") == 0 &&
			 (pg_strcasecmp(prev2_wd, "ALTER") == 0 ||
			  pg_strcasecmp(prev2_wd, "DROP") == 0 ||
			  pg_strcasecmp(prev2_wd, "RENAME") == 0) &&
			 pg_strcasecmp(prev_wd, "ATTRIBUTE") == 0)
		COMPLETE_WITH_ATTR(prev3_wd, "");
	/* ALTER TYPE ALTER ATTRIBUTE <foo> */
	else if ((pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			  pg_strcasecmp(prev2_wd, "ATTRIBUTE") == 0))
	{
		COMPLETE_WITH_CONST("TYPE");
	}
	/* complete ALTER GROUP <foo> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "GROUP") == 0)
	{
		static const char *const list_ALTERGROUP[] =
		{"ADD USER", "DROP USER", "RENAME TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERGROUP);
	}
	/* complete ALTER GROUP <foo> ADD|DROP with USER */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "GROUP") == 0 &&
			 (pg_strcasecmp(prev_wd, "ADD") == 0 ||
			  pg_strcasecmp(prev_wd, "DROP") == 0))
		COMPLETE_WITH_CONST("USER");
	/* complete {ALTER} GROUP <foo> ADD|DROP USER with a user name */
	else if (pg_strcasecmp(prev4_wd, "GROUP") == 0 &&
			 (pg_strcasecmp(prev2_wd, "ADD") == 0 ||
			  pg_strcasecmp(prev2_wd, "DROP") == 0) &&
			 pg_strcasecmp(prev_wd, "USER") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);

/* BEGIN, END, ABORT */
	else if (pg_strcasecmp(prev_wd, "BEGIN") == 0 ||
			 pg_strcasecmp(prev_wd, "END") == 0 ||
			 pg_strcasecmp(prev_wd, "ABORT") == 0)
	{
		static const char *const list_TRANS[] =
		{"WORK", "TRANSACTION", NULL};

		COMPLETE_WITH_LIST(list_TRANS);
	}
/* COMMIT */
	else if (pg_strcasecmp(prev_wd, "COMMIT") == 0)
	{
		static const char *const list_COMMIT[] =
		{"WORK", "TRANSACTION", "PREPARED", NULL};

		COMPLETE_WITH_LIST(list_COMMIT);
	}
/* RELEASE SAVEPOINT */
	else if (pg_strcasecmp(prev_wd, "RELEASE") == 0)
		COMPLETE_WITH_CONST("SAVEPOINT");
/* ROLLBACK*/
	else if (pg_strcasecmp(prev_wd, "ROLLBACK") == 0)
	{
		static const char *const list_TRANS[] =
		{"WORK", "TRANSACTION", "TO SAVEPOINT", "PREPARED", NULL};

		COMPLETE_WITH_LIST(list_TRANS);
	}
/* CLUSTER */

	/*
	 * If the previous word is CLUSTER and not WITHOUT produce list of tables
	 */
	else if (pg_strcasecmp(prev_wd, "CLUSTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "WITHOUT") != 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm, "UNION SELECT 'VERBOSE'");

	/*
	 * If the previous words are CLUSTER VERBOSE produce list of tables
	 */
	else if (pg_strcasecmp(prev_wd, "VERBOSE") == 0 &&
			 pg_strcasecmp(prev2_wd, "CLUSTER") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm, NULL);

	/* If we have CLUSTER <sth>, then add "USING" */
	else if (pg_strcasecmp(prev2_wd, "CLUSTER") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") != 0 &&
			 pg_strcasecmp(prev_wd, "VERBOSE") != 0)
	{
		COMPLETE_WITH_CONST("USING");
	}
	/* If we have CLUSTER VERBOSE <sth>, then add "USING" */
	else if (pg_strcasecmp(prev3_wd, "CLUSTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "VERBOSE") == 0)
	{
		COMPLETE_WITH_CONST("USING");
	}

	/*
	 * If we have CLUSTER <sth> USING, then add the index as well.
	 */
	else if (pg_strcasecmp(prev3_wd, "CLUSTER") == 0 &&
			 pg_strcasecmp(prev_wd, "USING") == 0)
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_index_of_table);
	}

	/*
	 * If we have CLUSTER VERBOSE <sth> USING, then add the index as well.
	 */
	else if (pg_strcasecmp(prev4_wd, "CLUSTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "VERBOSE") == 0 &&
			 pg_strcasecmp(prev_wd, "USING") == 0)
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_index_of_table);
	}

/* COMMENT */
	else if (pg_strcasecmp(prev_wd, "COMMENT") == 0)
		COMPLETE_WITH_CONST("ON");
	else if (pg_strcasecmp(prev2_wd, "COMMENT") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
	{
		static const char *const list_COMMENT[] =
		{"CAST", "COLLATION", "CONVERSION", "DATABASE", "EVENT TRIGGER", "EXTENSION",
			"FOREIGN DATA WRAPPER", "FOREIGN TABLE",
			"SERVER", "INDEX", "LANGUAGE", "RULE", "SCHEMA", "SEQUENCE",
			"TABLE", "TYPE", "VIEW", "MATERIALIZED VIEW", "COLUMN", "AGGREGATE", "FUNCTION",
			"OPERATOR", "TRIGGER", "CONSTRAINT", "DOMAIN", "LARGE OBJECT",
		"TABLESPACE", "TEXT SEARCH", "ROLE", NULL};

		COMPLETE_WITH_LIST(list_COMMENT);
	}
	else if (pg_strcasecmp(prev3_wd, "COMMENT") == 0 &&
			 pg_strcasecmp(prev2_wd, "ON") == 0 &&
			 pg_strcasecmp(prev_wd, "FOREIGN") == 0)
	{
		static const char *const list_TRANS2[] =
		{"DATA WRAPPER", "TABLE", NULL};

		COMPLETE_WITH_LIST(list_TRANS2);
	}
	else if (pg_strcasecmp(prev4_wd, "COMMENT") == 0 &&
			 pg_strcasecmp(prev3_wd, "ON") == 0 &&
			 pg_strcasecmp(prev2_wd, "TEXT") == 0 &&
			 pg_strcasecmp(prev_wd, "SEARCH") == 0)
	{
		static const char *const list_TRANS2[] =
		{"CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE", NULL};

		COMPLETE_WITH_LIST(list_TRANS2);
	}
	else if (pg_strcasecmp(prev3_wd, "COMMENT") == 0 &&
			 pg_strcasecmp(prev2_wd, "ON") == 0 &&
			 pg_strcasecmp(prev_wd, "CONSTRAINT") == 0)
	{
		COMPLETE_WITH_QUERY(Query_for_all_table_constraints);
	}
	else if (pg_strcasecmp(prev4_wd, "COMMENT") == 0 &&
			 pg_strcasecmp(prev3_wd, "ON") == 0 &&
			 pg_strcasecmp(prev2_wd, "CONSTRAINT") == 0)
	{
		COMPLETE_WITH_CONST("ON");
	}
	else if (pg_strcasecmp(prev5_wd, "COMMENT") == 0 &&
			 pg_strcasecmp(prev4_wd, "ON") == 0 &&
			 pg_strcasecmp(prev3_wd, "CONSTRAINT") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
	{
		completion_info_charp = prev2_wd;
		COMPLETE_WITH_QUERY(Query_for_list_of_tables_for_constraint);
	}
	else if (pg_strcasecmp(prev4_wd, "COMMENT") == 0 &&
			 pg_strcasecmp(prev3_wd, "ON") == 0 &&
			 pg_strcasecmp(prev2_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev_wd, "VIEW") == 0)
	{
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews, NULL);
	}
	else if (pg_strcasecmp(prev4_wd, "COMMENT") == 0 &&
			 pg_strcasecmp(prev3_wd, "ON") == 0 &&
			 pg_strcasecmp(prev2_wd, "EVENT") == 0 &&
			 pg_strcasecmp(prev_wd, "TRIGGER") == 0)
	{
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);
	}
	else if ((pg_strcasecmp(prev4_wd, "COMMENT") == 0 &&
			  pg_strcasecmp(prev3_wd, "ON") == 0) ||
			 (pg_strcasecmp(prev5_wd, "COMMENT") == 0 &&
			  pg_strcasecmp(prev4_wd, "ON") == 0) ||
			 (pg_strcasecmp(prev6_wd, "COMMENT") == 0 &&
			  pg_strcasecmp(prev5_wd, "ON") == 0))
		COMPLETE_WITH_CONST("IS");

/* COPY */

	/*
	 * If we have COPY [BINARY] (which you'd have to type yourself), offer
	 * list of tables (Also cover the analogous backslash command)
	 */
	else if (pg_strcasecmp(prev_wd, "COPY") == 0 ||
			 pg_strcasecmp(prev_wd, "\\copy") == 0 ||
			 (pg_strcasecmp(prev2_wd, "COPY") == 0 &&
			  pg_strcasecmp(prev_wd, "BINARY") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
	/* If we have COPY|BINARY <sth>, complete it with "TO" or "FROM" */
	else if (pg_strcasecmp(prev2_wd, "COPY") == 0 ||
			 pg_strcasecmp(prev2_wd, "\\copy") == 0 ||
			 pg_strcasecmp(prev2_wd, "BINARY") == 0)
	{
		static const char *const list_FROMTO[] =
		{"FROM", "TO", NULL};

		COMPLETE_WITH_LIST(list_FROMTO);
	}
	/* If we have COPY|BINARY <sth> FROM|TO, complete with filename */
	else if ((pg_strcasecmp(prev3_wd, "COPY") == 0 ||
			  pg_strcasecmp(prev3_wd, "\\copy") == 0 ||
			  pg_strcasecmp(prev3_wd, "BINARY") == 0) &&
			 (pg_strcasecmp(prev_wd, "FROM") == 0 ||
			  pg_strcasecmp(prev_wd, "TO") == 0))
	{
		completion_charp = "";
		matches = completion_matches(text, complete_from_files);
	}

	/* Handle COPY|BINARY <sth> FROM|TO filename */
	else if ((pg_strcasecmp(prev4_wd, "COPY") == 0 ||
			  pg_strcasecmp(prev4_wd, "\\copy") == 0 ||
			  pg_strcasecmp(prev4_wd, "BINARY") == 0) &&
			 (pg_strcasecmp(prev2_wd, "FROM") == 0 ||
			  pg_strcasecmp(prev2_wd, "TO") == 0))
	{
		static const char *const list_COPY[] =
		{"BINARY", "OIDS", "DELIMITER", "NULL", "CSV", "ENCODING", NULL};

		COMPLETE_WITH_LIST(list_COPY);
	}

	/* Handle COPY|BINARY <sth> FROM|TO filename CSV */
	else if (pg_strcasecmp(prev_wd, "CSV") == 0 &&
			 (pg_strcasecmp(prev3_wd, "FROM") == 0 ||
			  pg_strcasecmp(prev3_wd, "TO") == 0))
	{
		static const char *const list_CSV[] =
		{"HEADER", "QUOTE", "ESCAPE", "FORCE QUOTE", "FORCE NOT NULL", NULL};

		COMPLETE_WITH_LIST(list_CSV);
	}

	/* CREATE DATABASE */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev2_wd, "DATABASE") == 0)
	{
		static const char *const list_DATABASE[] =
		{"OWNER", "TEMPLATE", "ENCODING", "TABLESPACE", "CONNECTION LIMIT",
		"LC_COLLATE", "LC_CTYPE", NULL};

		COMPLETE_WITH_LIST(list_DATABASE);
	}

	else if (pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev3_wd, "DATABASE") == 0 &&
			 pg_strcasecmp(prev_wd, "TEMPLATE") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_template_databases);

	/* CREATE EXTENSION */
	/* Complete with available extensions rather than installed ones. */
	else if (pg_strcasecmp(prev2_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev_wd, "EXTENSION") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_available_extensions);
	/* CREATE EXTENSION <name> */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev2_wd, "EXTENSION") == 0)
		COMPLETE_WITH_CONST("WITH SCHEMA");

	/* CREATE FOREIGN */
	else if (pg_strcasecmp(prev2_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev_wd, "FOREIGN") == 0)
	{
		static const char *const list_CREATE_FOREIGN[] =
		{"DATA WRAPPER", "TABLE", NULL};

		COMPLETE_WITH_LIST(list_CREATE_FOREIGN);
	}

	/* CREATE FOREIGN DATA WRAPPER */
	else if (pg_strcasecmp(prev5_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev4_wd, "FOREIGN") == 0 &&
			 pg_strcasecmp(prev3_wd, "DATA") == 0 &&
			 pg_strcasecmp(prev2_wd, "WRAPPER") == 0)
	{
		static const char *const list_CREATE_FOREIGN_DATA_WRAPPER[] =
		{"HANDLER", "VALIDATOR", NULL};

		COMPLETE_WITH_LIST(list_CREATE_FOREIGN_DATA_WRAPPER);
	}

	/* CREATE INDEX */
	/* First off we complete CREATE UNIQUE with "INDEX" */
	else if (pg_strcasecmp(prev2_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev_wd, "UNIQUE") == 0)
		COMPLETE_WITH_CONST("INDEX");
	/* If we have CREATE|UNIQUE INDEX, then add "ON" and existing indexes */
	else if (pg_strcasecmp(prev_wd, "INDEX") == 0 &&
			 (pg_strcasecmp(prev2_wd, "CREATE") == 0 ||
			  pg_strcasecmp(prev2_wd, "UNIQUE") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes,
								   " UNION SELECT 'ON'"
								   " UNION SELECT 'CONCURRENTLY'");
	/* Complete ... INDEX [<name>] ON with a list of tables  */
	else if ((pg_strcasecmp(prev3_wd, "INDEX") == 0 ||
			  pg_strcasecmp(prev2_wd, "INDEX") == 0 ||
			  pg_strcasecmp(prev2_wd, "CONCURRENTLY") == 0) &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm, NULL);
	/* If we have CREATE|UNIQUE INDEX <sth> CONCURRENTLY, then add "ON" */
	else if ((pg_strcasecmp(prev3_wd, "INDEX") == 0 ||
			  pg_strcasecmp(prev2_wd, "INDEX") == 0) &&
			 pg_strcasecmp(prev_wd, "CONCURRENTLY") == 0)
		COMPLETE_WITH_CONST("ON");
	/* If we have CREATE|UNIQUE INDEX <sth>, then add "ON" or "CONCURRENTLY" */
	else if ((pg_strcasecmp(prev3_wd, "CREATE") == 0 ||
			  pg_strcasecmp(prev3_wd, "UNIQUE") == 0) &&
			 pg_strcasecmp(prev2_wd, "INDEX") == 0)
	{
		static const char *const list_CREATE_INDEX[] =
		{"CONCURRENTLY", "ON", NULL};

		COMPLETE_WITH_LIST(list_CREATE_INDEX);
	}

	/*
	 * Complete INDEX <name> ON <table> with a list of table columns (which
	 * should really be in parens)
	 */
	else if ((pg_strcasecmp(prev4_wd, "INDEX") == 0 ||
			  pg_strcasecmp(prev3_wd, "INDEX") == 0 ||
			  pg_strcasecmp(prev3_wd, "CONCURRENTLY") == 0) &&
			 pg_strcasecmp(prev2_wd, "ON") == 0)
	{
		static const char *const list_CREATE_INDEX2[] =
		{"(", "USING", NULL};

		COMPLETE_WITH_LIST(list_CREATE_INDEX2);
	}
	else if ((pg_strcasecmp(prev5_wd, "INDEX") == 0 ||
			  pg_strcasecmp(prev4_wd, "INDEX") == 0 ||
			  pg_strcasecmp(prev4_wd, "CONCURRENTLY") == 0) &&
			 pg_strcasecmp(prev3_wd, "ON") == 0 &&
			 pg_strcasecmp(prev_wd, "(") == 0)
		COMPLETE_WITH_ATTR(prev2_wd, "");
	/* same if you put in USING */
	else if (pg_strcasecmp(prev5_wd, "ON") == 0 &&
			 pg_strcasecmp(prev3_wd, "USING") == 0 &&
			 pg_strcasecmp(prev_wd, "(") == 0)
		COMPLETE_WITH_ATTR(prev4_wd, "");
	/* Complete USING with an index method */
	else if (pg_strcasecmp(prev_wd, "USING") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_access_methods);
	else if (pg_strcasecmp(prev4_wd, "ON") == 0 &&
			 pg_strcasecmp(prev2_wd, "USING") == 0)
		COMPLETE_WITH_CONST("(");

/* CREATE RULE */
	/* Complete "CREATE RULE <sth>" with "AS" */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev2_wd, "RULE") == 0)
		COMPLETE_WITH_CONST("AS");
	/* Complete "CREATE RULE <sth> AS with "ON" */
	else if (pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev3_wd, "RULE") == 0 &&
			 pg_strcasecmp(prev_wd, "AS") == 0)
		COMPLETE_WITH_CONST("ON");
	/* Complete "RULE * AS ON" with SELECT|UPDATE|DELETE|INSERT */
	else if (pg_strcasecmp(prev4_wd, "RULE") == 0 &&
			 pg_strcasecmp(prev2_wd, "AS") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
	{
		static const char *const rule_events[] =
		{"SELECT", "UPDATE", "INSERT", "DELETE", NULL};

		COMPLETE_WITH_LIST(rule_events);
	}
	/* Complete "AS ON <sth with a 'T' :)>" with a "TO" */
	else if (pg_strcasecmp(prev3_wd, "AS") == 0 &&
			 pg_strcasecmp(prev2_wd, "ON") == 0 &&
			 (pg_toupper((unsigned char) prev_wd[4]) == 'T' ||
			  pg_toupper((unsigned char) prev_wd[5]) == 'T'))
		COMPLETE_WITH_CONST("TO");
	/* Complete "AS ON <sth> TO" with a table name */
	else if (pg_strcasecmp(prev4_wd, "AS") == 0 &&
			 pg_strcasecmp(prev3_wd, "ON") == 0 &&
			 pg_strcasecmp(prev_wd, "TO") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);

/* CREATE SERVER <name> */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev2_wd, "SERVER") == 0)
	{
		static const char *const list_CREATE_SERVER[] =
		{"TYPE", "VERSION", "FOREIGN DATA WRAPPER", NULL};

		COMPLETE_WITH_LIST(list_CREATE_SERVER);
	}

/* CREATE TABLE */
	/* Complete "CREATE TEMP/TEMPORARY" with the possible temp objects */
	else if (pg_strcasecmp(prev2_wd, "CREATE") == 0 &&
			 (pg_strcasecmp(prev_wd, "TEMP") == 0 ||
			  pg_strcasecmp(prev_wd, "TEMPORARY") == 0))
	{
		static const char *const list_TEMP[] =
		{"SEQUENCE", "TABLE", "VIEW", NULL};

		COMPLETE_WITH_LIST(list_TEMP);
	}
	/* Complete "CREATE UNLOGGED" with TABLE */
	else if (pg_strcasecmp(prev2_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev_wd, "UNLOGGED") == 0)
	{
		static const char *const list_UNLOGGED[] =
		{"TABLE", "MATERIALIZED VIEW", NULL};

		COMPLETE_WITH_LIST(list_UNLOGGED);
	}

/* CREATE TABLESPACE */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev2_wd, "TABLESPACE") == 0)
	{
		static const char *const list_CREATETABLESPACE[] =
		{"OWNER", "LOCATION", NULL};

		COMPLETE_WITH_LIST(list_CREATETABLESPACE);
	}
	/* Complete CREATE TABLESPACE name OWNER name with "LOCATION" */
	else if (pg_strcasecmp(prev5_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev4_wd, "TABLESPACE") == 0 &&
			 pg_strcasecmp(prev2_wd, "OWNER") == 0)
	{
		COMPLETE_WITH_CONST("LOCATION");
	}

/* CREATE TEXT SEARCH */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev2_wd, "TEXT") == 0 &&
			 pg_strcasecmp(prev_wd, "SEARCH") == 0)
	{
		static const char *const list_CREATETEXTSEARCH[] =
		{"CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE", NULL};

		COMPLETE_WITH_LIST(list_CREATETEXTSEARCH);
	}
	else if (pg_strcasecmp(prev4_wd, "TEXT") == 0 &&
			 pg_strcasecmp(prev3_wd, "SEARCH") == 0 &&
			 pg_strcasecmp(prev2_wd, "CONFIGURATION") == 0)
		COMPLETE_WITH_CONST("(");

/* CREATE TRIGGER */
	/* complete CREATE TRIGGER <name> with BEFORE,AFTER */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev2_wd, "TRIGGER") == 0)
	{
		static const char *const list_CREATETRIGGER[] =
		{"BEFORE", "AFTER", "INSTEAD OF", NULL};

		COMPLETE_WITH_LIST(list_CREATETRIGGER);
	}
	/* complete CREATE TRIGGER <name> BEFORE,AFTER with an event */
	else if (pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev3_wd, "TRIGGER") == 0 &&
			 (pg_strcasecmp(prev_wd, "BEFORE") == 0 ||
			  pg_strcasecmp(prev_wd, "AFTER") == 0))
	{
		static const char *const list_CREATETRIGGER_EVENTS[] =
		{"INSERT", "DELETE", "UPDATE", "TRUNCATE", NULL};

		COMPLETE_WITH_LIST(list_CREATETRIGGER_EVENTS);
	}
	/* complete CREATE TRIGGER <name> INSTEAD OF with an event */
	else if (pg_strcasecmp(prev5_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev4_wd, "TRIGGER") == 0 &&
			 pg_strcasecmp(prev2_wd, "INSTEAD") == 0 &&
			 pg_strcasecmp(prev_wd, "OF") == 0)
	{
		static const char *const list_CREATETRIGGER_EVENTS[] =
		{"INSERT", "DELETE", "UPDATE", NULL};

		COMPLETE_WITH_LIST(list_CREATETRIGGER_EVENTS);
	}
	/* complete CREATE TRIGGER <name> BEFORE,AFTER sth with OR,ON */
	else if ((pg_strcasecmp(prev5_wd, "CREATE") == 0 &&
			  pg_strcasecmp(prev4_wd, "TRIGGER") == 0 &&
			  (pg_strcasecmp(prev2_wd, "BEFORE") == 0 ||
			   pg_strcasecmp(prev2_wd, "AFTER") == 0)) ||
			 (pg_strcasecmp(prev5_wd, "TRIGGER") == 0 &&
			  pg_strcasecmp(prev3_wd, "INSTEAD") == 0 &&
			  pg_strcasecmp(prev2_wd, "OF") == 0))
	{
		static const char *const list_CREATETRIGGER2[] =
		{"ON", "OR", NULL};

		COMPLETE_WITH_LIST(list_CREATETRIGGER2);
	}

	/*
	 * complete CREATE TRIGGER <name> BEFORE,AFTER event ON with a list of
	 * tables
	 */
	else if (pg_strcasecmp(prev5_wd, "TRIGGER") == 0 &&
			 (pg_strcasecmp(prev3_wd, "BEFORE") == 0 ||
			  pg_strcasecmp(prev3_wd, "AFTER") == 0) &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
	/* complete CREATE TRIGGER ... INSTEAD OF event ON with a list of views */
	else if (pg_strcasecmp(prev4_wd, "INSTEAD") == 0 &&
			 pg_strcasecmp(prev3_wd, "OF") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views, NULL);
	/* complete CREATE TRIGGER ... EXECUTE with PROCEDURE */
	else if (pg_strcasecmp(prev_wd, "EXECUTE") == 0 &&
			 prev2_wd[0] != '\0')
		COMPLETE_WITH_CONST("PROCEDURE");

/* CREATE ROLE,USER,GROUP <name> */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 !(pg_strcasecmp(prev2_wd, "USER") == 0 && pg_strcasecmp(prev_wd, "MAPPING") == 0) &&
			 (pg_strcasecmp(prev2_wd, "ROLE") == 0 ||
			  pg_strcasecmp(prev2_wd, "GROUP") == 0 || pg_strcasecmp(prev2_wd, "USER") == 0))
	{
		static const char *const list_CREATEROLE[] =
		{"ADMIN", "CONNECTION LIMIT", "CREATEDB", "CREATEROLE", "CREATEUSER",
			"ENCRYPTED", "IN", "INHERIT", "LOGIN", "NOCREATEDB",
			"NOCREATEROLE", "NOCREATEUSER", "NOINHERIT", "NOLOGIN",
			"NOREPLICATION", "NOSUPERUSER", "REPLICATION", "ROLE",
		"SUPERUSER", "SYSID", "UNENCRYPTED", "VALID UNTIL", "WITH", NULL};

		COMPLETE_WITH_LIST(list_CREATEROLE);
	}

/* CREATE ROLE,USER,GROUP <name> WITH */
	else if ((pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			  (pg_strcasecmp(prev3_wd, "ROLE") == 0 ||
			   pg_strcasecmp(prev3_wd, "GROUP") == 0 ||
			   pg_strcasecmp(prev3_wd, "USER") == 0) &&
			  pg_strcasecmp(prev_wd, "WITH") == 0))
	{
		/* Similar to the above, but don't complete "WITH" again. */
		static const char *const list_CREATEROLE_WITH[] =
		{"ADMIN", "CONNECTION LIMIT", "CREATEDB", "CREATEROLE", "CREATEUSER",
			"ENCRYPTED", "IN", "INHERIT", "LOGIN", "NOCREATEDB",
			"NOCREATEROLE", "NOCREATEUSER", "NOINHERIT", "NOLOGIN",
			"NOREPLICATION", "NOSUPERUSER", "REPLICATION", "ROLE",
		"SUPERUSER", "SYSID", "UNENCRYPTED", "VALID UNTIL", NULL};

		COMPLETE_WITH_LIST(list_CREATEROLE_WITH);
	}

	/*
	 * complete CREATE ROLE,USER,GROUP <name> ENCRYPTED,UNENCRYPTED with
	 * PASSWORD
	 */
	else if (pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			 (pg_strcasecmp(prev3_wd, "ROLE") == 0 ||
			  pg_strcasecmp(prev3_wd, "GROUP") == 0 || pg_strcasecmp(prev3_wd, "USER") == 0) &&
			 (pg_strcasecmp(prev_wd, "ENCRYPTED") == 0 || pg_strcasecmp(prev_wd, "UNENCRYPTED") == 0))
	{
		COMPLETE_WITH_CONST("PASSWORD");
	}
	/* complete CREATE ROLE,USER,GROUP <name> IN with ROLE,GROUP */
	else if (pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			 (pg_strcasecmp(prev3_wd, "ROLE") == 0 ||
			  pg_strcasecmp(prev3_wd, "GROUP") == 0 || pg_strcasecmp(prev3_wd, "USER") == 0) &&
			 pg_strcasecmp(prev_wd, "IN") == 0)
	{
		static const char *const list_CREATEROLE3[] =
		{"GROUP", "ROLE", NULL};

		COMPLETE_WITH_LIST(list_CREATEROLE3);
	}

/* CREATE VIEW */
	/* Complete CREATE VIEW <name> with AS */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev2_wd, "VIEW") == 0)
		COMPLETE_WITH_CONST("AS");
	/* Complete "CREATE VIEW <sth> AS with "SELECT" */
	else if (pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev3_wd, "VIEW") == 0 &&
			 pg_strcasecmp(prev_wd, "AS") == 0)
		COMPLETE_WITH_CONST("SELECT");

/* CREATE MATERIALIZED VIEW */
	else if (pg_strcasecmp(prev2_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev_wd, "MATERIALIZED") == 0)
		COMPLETE_WITH_CONST("VIEW");
	/* Complete CREATE MATERIALIZED VIEW <name> with AS */
	else if (pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev3_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev2_wd, "VIEW") == 0)
		COMPLETE_WITH_CONST("AS");
	/* Complete "CREATE MATERIALIZED VIEW <sth> AS with "SELECT" */
	else if (pg_strcasecmp(prev5_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev4_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev3_wd, "VIEW") == 0 &&
			 pg_strcasecmp(prev_wd, "AS") == 0)
		COMPLETE_WITH_CONST("SELECT");

/* CREATE EVENT TRIGGER */
	else if (pg_strcasecmp(prev2_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev_wd, "EVENT") == 0)
		COMPLETE_WITH_CONST("TRIGGER");
	/* Complete CREATE EVENT TRIGGER <name> with ON */
	else if (pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev3_wd, "EVENT") == 0 &&
			 pg_strcasecmp(prev2_wd, "TRIGGER") == 0)
		COMPLETE_WITH_CONST("ON");
	/* Complete CREATE EVENT TRIGGER <name> ON with event_type */
	else if (pg_strcasecmp(prev5_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev4_wd, "EVENT") == 0 &&
			 pg_strcasecmp(prev3_wd, "TRIGGER") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
	{
		static const char *const list_CREATE_EVENT_TRIGGER_ON[] =
		{"ddl_command_start", "ddl_command_end", "sql_drop", NULL};

		COMPLETE_WITH_LIST(list_CREATE_EVENT_TRIGGER_ON);
	}

/* DECLARE */
	else if (pg_strcasecmp(prev2_wd, "DECLARE") == 0)
	{
		static const char *const list_DECLARE[] =
		{"BINARY", "INSENSITIVE", "SCROLL", "NO SCROLL", "CURSOR", NULL};

		COMPLETE_WITH_LIST(list_DECLARE);
	}

/* CURSOR */
	else if (pg_strcasecmp(prev_wd, "CURSOR") == 0)
	{
		static const char *const list_DECLARECURSOR[] =
		{"WITH HOLD", "WITHOUT HOLD", "FOR", NULL};

		COMPLETE_WITH_LIST(list_DECLARECURSOR);
	}


/* DELETE */

	/*
	 * Complete DELETE with FROM (only if the word before that is not "ON"
	 * (cf. rules) or "BEFORE" or "AFTER" (cf. triggers) or GRANT)
	 */
	else if (pg_strcasecmp(prev_wd, "DELETE") == 0 &&
			 !(pg_strcasecmp(prev2_wd, "ON") == 0 ||
			   pg_strcasecmp(prev2_wd, "GRANT") == 0 ||
			   pg_strcasecmp(prev2_wd, "BEFORE") == 0 ||
			   pg_strcasecmp(prev2_wd, "AFTER") == 0))
		COMPLETE_WITH_CONST("FROM");
	/* Complete DELETE FROM with a list of tables */
	else if (pg_strcasecmp(prev2_wd, "DELETE") == 0 &&
			 pg_strcasecmp(prev_wd, "FROM") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_updatables, NULL);
	/* Complete DELETE FROM <table> */
	else if (pg_strcasecmp(prev3_wd, "DELETE") == 0 &&
			 pg_strcasecmp(prev2_wd, "FROM") == 0)
	{
		static const char *const list_DELETE[] =
		{"USING", "WHERE", "SET", NULL};

		COMPLETE_WITH_LIST(list_DELETE);
	}
	/* XXX: implement tab completion for DELETE ... USING */

/* DISCARD */
	else if (pg_strcasecmp(prev_wd, "DISCARD") == 0)
	{
		static const char *const list_DISCARD[] =
		{"ALL", "PLANS", "SEQUENCES", "TEMP", NULL};

		COMPLETE_WITH_LIST(list_DISCARD);
	}

/* DO */

	/*
	 * Complete DO with LANGUAGE.
	 */
	else if (pg_strcasecmp(prev_wd, "DO") == 0)
	{
		static const char *const list_DO[] =
		{"LANGUAGE", NULL};

		COMPLETE_WITH_LIST(list_DO);
	}

/* DROP (when not the previous word) */
	/* DROP AGGREGATE */
	else if (pg_strcasecmp(prev3_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev2_wd, "AGGREGATE") == 0)
		COMPLETE_WITH_CONST("(");

	/* DROP object with CASCADE / RESTRICT */
	else if ((pg_strcasecmp(prev3_wd, "DROP") == 0 &&
			  (pg_strcasecmp(prev2_wd, "COLLATION") == 0 ||
			   pg_strcasecmp(prev2_wd, "CONVERSION") == 0 ||
			   pg_strcasecmp(prev2_wd, "DOMAIN") == 0 ||
			   pg_strcasecmp(prev2_wd, "EXTENSION") == 0 ||
			   pg_strcasecmp(prev2_wd, "FUNCTION") == 0 ||
			   pg_strcasecmp(prev2_wd, "INDEX") == 0 ||
			   pg_strcasecmp(prev2_wd, "LANGUAGE") == 0 ||
			   pg_strcasecmp(prev2_wd, "SCHEMA") == 0 ||
			   pg_strcasecmp(prev2_wd, "SEQUENCE") == 0 ||
			   pg_strcasecmp(prev2_wd, "SERVER") == 0 ||
			   pg_strcasecmp(prev2_wd, "TABLE") == 0 ||
			   pg_strcasecmp(prev2_wd, "TYPE") == 0 ||
			   pg_strcasecmp(prev2_wd, "VIEW") == 0)) ||
			 (pg_strcasecmp(prev4_wd, "DROP") == 0 &&
			  pg_strcasecmp(prev3_wd, "AGGREGATE") == 0 &&
			  prev_wd[strlen(prev_wd) - 1] == ')') ||
			 (pg_strcasecmp(prev4_wd, "DROP") == 0 &&
			  pg_strcasecmp(prev3_wd, "EVENT") == 0 &&
			  pg_strcasecmp(prev2_wd, "TRIGGER") == 0) ||
			 (pg_strcasecmp(prev5_wd, "DROP") == 0 &&
			  pg_strcasecmp(prev4_wd, "FOREIGN") == 0 &&
			  pg_strcasecmp(prev3_wd, "DATA") == 0 &&
			  pg_strcasecmp(prev2_wd, "WRAPPER") == 0) ||
			 (pg_strcasecmp(prev5_wd, "DROP") == 0 &&
			  pg_strcasecmp(prev4_wd, "TEXT") == 0 &&
			  pg_strcasecmp(prev3_wd, "SEARCH") == 0 &&
			  (pg_strcasecmp(prev2_wd, "CONFIGURATION") == 0 ||
			   pg_strcasecmp(prev2_wd, "DICTIONARY") == 0 ||
			   pg_strcasecmp(prev2_wd, "PARSER") == 0 ||
			   pg_strcasecmp(prev2_wd, "TEMPLATE") == 0))
		)
	{
		if (pg_strcasecmp(prev3_wd, "DROP") == 0 &&
			pg_strcasecmp(prev2_wd, "FUNCTION") == 0)
		{
			COMPLETE_WITH_CONST("(");
		}
		else
		{
			static const char *const list_DROPCR[] =
			{"CASCADE", "RESTRICT", NULL};

			COMPLETE_WITH_LIST(list_DROPCR);
		}
	}
	else if (pg_strcasecmp(prev2_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev_wd, "FOREIGN") == 0)
	{
		static const char *const drop_CREATE_FOREIGN[] =
		{"DATA WRAPPER", "TABLE", NULL};

		COMPLETE_WITH_LIST(drop_CREATE_FOREIGN);
	}

	/* DROP MATERIALIZED VIEW */
	else if (pg_strcasecmp(prev2_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev_wd, "MATERIALIZED") == 0)
	{
		COMPLETE_WITH_CONST("VIEW");
	}
	else if (pg_strcasecmp(prev3_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev2_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev_wd, "VIEW") == 0)
	{
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews, NULL);
	}

	else if (pg_strcasecmp(prev4_wd, "DROP") == 0 &&
			 (pg_strcasecmp(prev3_wd, "AGGREGATE") == 0 ||
			  pg_strcasecmp(prev3_wd, "FUNCTION") == 0) &&
			 pg_strcasecmp(prev_wd, "(") == 0)
		COMPLETE_WITH_FUNCTION_ARG(prev2_wd);
	/* DROP OWNED BY */
	else if (pg_strcasecmp(prev2_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev_wd, "OWNED") == 0)
		COMPLETE_WITH_CONST("BY");
	else if (pg_strcasecmp(prev3_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev2_wd, "OWNED") == 0 &&
			 pg_strcasecmp(prev_wd, "BY") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (pg_strcasecmp(prev3_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev2_wd, "TEXT") == 0 &&
			 pg_strcasecmp(prev_wd, "SEARCH") == 0)
	{

		static const char *const list_ALTERTEXTSEARCH[] =
		{"CONFIGURATION", "DICTIONARY", "PARSER", "TEMPLATE", NULL};

		COMPLETE_WITH_LIST(list_ALTERTEXTSEARCH);
	}

	/* DROP EVENT TRIGGER */
	else if (pg_strcasecmp(prev2_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev_wd, "EVENT") == 0)
	{
		COMPLETE_WITH_CONST("TRIGGER");
	}
	else if (pg_strcasecmp(prev3_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev2_wd, "EVENT") == 0 &&
			 pg_strcasecmp(prev_wd, "TRIGGER") == 0)
	{
		COMPLETE_WITH_QUERY(Query_for_list_of_event_triggers);
	}

/* EXECUTE, but not EXECUTE embedded in other commands */
	else if (pg_strcasecmp(prev_wd, "EXECUTE") == 0 &&
			 prev2_wd[0] == '\0')
		COMPLETE_WITH_QUERY(Query_for_list_of_prepared_statements);

/* EXPLAIN */

	/*
	 * Complete EXPLAIN [ANALYZE] [VERBOSE] with list of EXPLAIN-able commands
	 */
	else if (pg_strcasecmp(prev_wd, "EXPLAIN") == 0)
	{
		static const char *const list_EXPLAIN[] =
		{"SELECT", "INSERT", "DELETE", "UPDATE", "DECLARE", "ANALYZE", "VERBOSE", NULL};

		COMPLETE_WITH_LIST(list_EXPLAIN);
	}
	else if (pg_strcasecmp(prev2_wd, "EXPLAIN") == 0 &&
			 pg_strcasecmp(prev_wd, "ANALYZE") == 0)
	{
		static const char *const list_EXPLAIN[] =
		{"SELECT", "INSERT", "DELETE", "UPDATE", "DECLARE", "VERBOSE", NULL};

		COMPLETE_WITH_LIST(list_EXPLAIN);
	}
	else if ((pg_strcasecmp(prev2_wd, "EXPLAIN") == 0 &&
			  pg_strcasecmp(prev_wd, "VERBOSE") == 0) ||
			 (pg_strcasecmp(prev3_wd, "EXPLAIN") == 0 &&
			  pg_strcasecmp(prev2_wd, "ANALYZE") == 0 &&
			  pg_strcasecmp(prev_wd, "VERBOSE") == 0))
	{
		static const char *const list_EXPLAIN[] =
		{"SELECT", "INSERT", "DELETE", "UPDATE", "DECLARE", NULL};

		COMPLETE_WITH_LIST(list_EXPLAIN);
	}

/* FETCH && MOVE */
	/* Complete FETCH with one of FORWARD, BACKWARD, RELATIVE */
	else if (pg_strcasecmp(prev_wd, "FETCH") == 0 ||
			 pg_strcasecmp(prev_wd, "MOVE") == 0)
	{
		static const char *const list_FETCH1[] =
		{"ABSOLUTE", "BACKWARD", "FORWARD", "RELATIVE", NULL};

		COMPLETE_WITH_LIST(list_FETCH1);
	}
	/* Complete FETCH <sth> with one of ALL, NEXT, PRIOR */
	else if (pg_strcasecmp(prev2_wd, "FETCH") == 0 ||
			 pg_strcasecmp(prev2_wd, "MOVE") == 0)
	{
		static const char *const list_FETCH2[] =
		{"ALL", "NEXT", "PRIOR", NULL};

		COMPLETE_WITH_LIST(list_FETCH2);
	}

	/*
	 * Complete FETCH <sth1> <sth2> with "FROM" or "IN". These are equivalent,
	 * but we may as well tab-complete both: perhaps some users prefer one
	 * variant or the other.
	 */
	else if (pg_strcasecmp(prev3_wd, "FETCH") == 0 ||
			 pg_strcasecmp(prev3_wd, "MOVE") == 0)
	{
		static const char *const list_FROMIN[] =
		{"FROM", "IN", NULL};

		COMPLETE_WITH_LIST(list_FROMIN);
	}

/* FOREIGN DATA WRAPPER */
	/* applies in ALTER/DROP FDW and in CREATE SERVER */
	else if (pg_strcasecmp(prev4_wd, "CREATE") != 0 &&
			 pg_strcasecmp(prev3_wd, "FOREIGN") == 0 &&
			 pg_strcasecmp(prev2_wd, "DATA") == 0 &&
			 pg_strcasecmp(prev_wd, "WRAPPER") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_fdws);

/* FOREIGN TABLE */
	else if (pg_strcasecmp(prev3_wd, "CREATE") != 0 &&
			 pg_strcasecmp(prev2_wd, "FOREIGN") == 0 &&
			 pg_strcasecmp(prev_wd, "TABLE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_foreign_tables, NULL);

/* GRANT && REVOKE */
	/* Complete GRANT/REVOKE with a list of roles and privileges */
	else if (pg_strcasecmp(prev_wd, "GRANT") == 0 ||
			 pg_strcasecmp(prev_wd, "REVOKE") == 0)
	{
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
	}

	/*
	 * Complete GRANT/REVOKE <privilege> with "ON", GRANT/REVOKE <role> with
	 * TO/FROM
	 */
	else if (pg_strcasecmp(prev2_wd, "GRANT") == 0 ||
			 pg_strcasecmp(prev2_wd, "REVOKE") == 0)
	{
		if (pg_strcasecmp(prev_wd, "SELECT") == 0
			|| pg_strcasecmp(prev_wd, "INSERT") == 0
			|| pg_strcasecmp(prev_wd, "UPDATE") == 0
			|| pg_strcasecmp(prev_wd, "DELETE") == 0
			|| pg_strcasecmp(prev_wd, "TRUNCATE") == 0
			|| pg_strcasecmp(prev_wd, "REFERENCES") == 0
			|| pg_strcasecmp(prev_wd, "TRIGGER") == 0
			|| pg_strcasecmp(prev_wd, "CREATE") == 0
			|| pg_strcasecmp(prev_wd, "CONNECT") == 0
			|| pg_strcasecmp(prev_wd, "TEMPORARY") == 0
			|| pg_strcasecmp(prev_wd, "TEMP") == 0
			|| pg_strcasecmp(prev_wd, "EXECUTE") == 0
			|| pg_strcasecmp(prev_wd, "USAGE") == 0
			|| pg_strcasecmp(prev_wd, "ALL") == 0)
			COMPLETE_WITH_CONST("ON");
		else
		{
			if (pg_strcasecmp(prev2_wd, "GRANT") == 0)
				COMPLETE_WITH_CONST("TO");
			else
				COMPLETE_WITH_CONST("FROM");
		}
	}

	/*
	 * Complete GRANT/REVOKE <sth> ON with a list of tables, views, sequences,
	 * and indexes
	 *
	 * keywords DATABASE, FUNCTION, LANGUAGE, SCHEMA added to query result via
	 * UNION; seems to work intuitively
	 *
	 * Note: GRANT/REVOKE can get quite complex; tab-completion as implemented
	 * here will only work if the privilege list contains exactly one
	 * privilege
	 */
	else if ((pg_strcasecmp(prev3_wd, "GRANT") == 0 ||
			  pg_strcasecmp(prev3_wd, "REVOKE") == 0) &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsvmf,
								   " UNION SELECT 'DATABASE'"
								   " UNION SELECT 'DOMAIN'"
								   " UNION SELECT 'FOREIGN DATA WRAPPER'"
								   " UNION SELECT 'FOREIGN SERVER'"
								   " UNION SELECT 'FUNCTION'"
								   " UNION SELECT 'LANGUAGE'"
								   " UNION SELECT 'LARGE OBJECT'"
								   " UNION SELECT 'SCHEMA'"
								   " UNION SELECT 'TABLESPACE'"
								   " UNION SELECT 'TYPE'");
	else if ((pg_strcasecmp(prev4_wd, "GRANT") == 0 ||
			  pg_strcasecmp(prev4_wd, "REVOKE") == 0) &&
			 pg_strcasecmp(prev2_wd, "ON") == 0 &&
			 pg_strcasecmp(prev_wd, "FOREIGN") == 0)
	{
		static const char *const list_privilege_foreign[] =
		{"DATA WRAPPER", "SERVER", NULL};

		COMPLETE_WITH_LIST(list_privilege_foreign);
	}

	/* Complete "GRANT/REVOKE * ON * " with "TO/FROM" */
	else if ((pg_strcasecmp(prev4_wd, "GRANT") == 0 ||
			  pg_strcasecmp(prev4_wd, "REVOKE") == 0) &&
			 pg_strcasecmp(prev2_wd, "ON") == 0)
	{
		if (pg_strcasecmp(prev_wd, "DATABASE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_databases);
		else if (pg_strcasecmp(prev_wd, "DOMAIN") == 0)
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_domains, NULL);
		else if (pg_strcasecmp(prev_wd, "FUNCTION") == 0)
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions, NULL);
		else if (pg_strcasecmp(prev_wd, "LANGUAGE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_languages);
		else if (pg_strcasecmp(prev_wd, "SCHEMA") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
		else if (pg_strcasecmp(prev_wd, "TABLESPACE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
		else if (pg_strcasecmp(prev_wd, "TYPE") == 0)
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes, NULL);
		else if (pg_strcasecmp(prev4_wd, "GRANT") == 0)
			COMPLETE_WITH_CONST("TO");
		else
			COMPLETE_WITH_CONST("FROM");
	}

	/* Complete "GRANT/REVOKE * ON * TO/FROM" with username, GROUP, or PUBLIC */
	else if (pg_strcasecmp(prev5_wd, "GRANT") == 0 &&
			 pg_strcasecmp(prev3_wd, "ON") == 0)
	{
		if (pg_strcasecmp(prev_wd, "TO") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_grant_roles);
		else
			COMPLETE_WITH_CONST("TO");
	}
	else if (pg_strcasecmp(prev5_wd, "REVOKE") == 0 &&
			 pg_strcasecmp(prev3_wd, "ON") == 0)
	{
		if (pg_strcasecmp(prev_wd, "FROM") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_grant_roles);
		else
			COMPLETE_WITH_CONST("FROM");
	}

	/* Complete "GRANT/REVOKE * TO/FROM" with username, GROUP, or PUBLIC */
	else if (pg_strcasecmp(prev3_wd, "GRANT") == 0 &&
			 pg_strcasecmp(prev_wd, "TO") == 0)
	{
		COMPLETE_WITH_QUERY(Query_for_list_of_grant_roles);
	}
	else if (pg_strcasecmp(prev3_wd, "REVOKE") == 0 &&
			 pg_strcasecmp(prev_wd, "FROM") == 0)
	{
		COMPLETE_WITH_QUERY(Query_for_list_of_grant_roles);
	}

/* GROUP BY */
	else if (pg_strcasecmp(prev3_wd, "FROM") == 0 &&
			 pg_strcasecmp(prev_wd, "GROUP") == 0)
		COMPLETE_WITH_CONST("BY");

/* INSERT */
	/* Complete INSERT with "INTO" */
	else if (pg_strcasecmp(prev_wd, "INSERT") == 0)
		COMPLETE_WITH_CONST("INTO");
	/* Complete INSERT INTO with table names */
	else if (pg_strcasecmp(prev2_wd, "INSERT") == 0 &&
			 pg_strcasecmp(prev_wd, "INTO") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_updatables, NULL);
	/* Complete "INSERT INTO <table> (" with attribute names */
	else if (pg_strcasecmp(prev4_wd, "INSERT") == 0 &&
			 pg_strcasecmp(prev3_wd, "INTO") == 0 &&
			 pg_strcasecmp(prev_wd, "(") == 0)
		COMPLETE_WITH_ATTR(prev2_wd, "");

	/*
	 * Complete INSERT INTO <table> with "(" or "VALUES" or "SELECT" or
	 * "TABLE" or "DEFAULT VALUES"
	 */
	else if (pg_strcasecmp(prev3_wd, "INSERT") == 0 &&
			 pg_strcasecmp(prev2_wd, "INTO") == 0)
	{
		static const char *const list_INSERT[] =
		{"(", "DEFAULT VALUES", "SELECT", "TABLE", "VALUES", NULL};

		COMPLETE_WITH_LIST(list_INSERT);
	}

	/*
	 * Complete INSERT INTO <table> (attribs) with "VALUES" or "SELECT" or
	 * "TABLE"
	 */
	else if (pg_strcasecmp(prev4_wd, "INSERT") == 0 &&
			 pg_strcasecmp(prev3_wd, "INTO") == 0 &&
			 prev_wd[strlen(prev_wd) - 1] == ')')
	{
		static const char *const list_INSERT[] =
		{"SELECT", "TABLE", "VALUES", NULL};

		COMPLETE_WITH_LIST(list_INSERT);
	}

	/* Insert an open parenthesis after "VALUES" */
	else if (pg_strcasecmp(prev_wd, "VALUES") == 0 &&
			 pg_strcasecmp(prev2_wd, "DEFAULT") != 0)
		COMPLETE_WITH_CONST("(");

/* LOCK */
	/* Complete LOCK [TABLE] with a list of tables */
	else if (pg_strcasecmp(prev_wd, "LOCK") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   " UNION SELECT 'TABLE'");
	else if (pg_strcasecmp(prev_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev2_wd, "LOCK") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, "");

	/* For the following, handle the case of a single table only for now */

	/* Complete LOCK [TABLE] <table> with "IN" */
	else if ((pg_strcasecmp(prev2_wd, "LOCK") == 0 &&
			  pg_strcasecmp(prev_wd, "TABLE") != 0) ||
			 (pg_strcasecmp(prev2_wd, "TABLE") == 0 &&
			  pg_strcasecmp(prev3_wd, "LOCK") == 0))
		COMPLETE_WITH_CONST("IN");

	/* Complete LOCK [TABLE] <table> IN with a lock mode */
	else if (pg_strcasecmp(prev_wd, "IN") == 0 &&
			 (pg_strcasecmp(prev3_wd, "LOCK") == 0 ||
			  (pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			   pg_strcasecmp(prev4_wd, "LOCK") == 0)))
	{
		static const char *const lock_modes[] =
		{"ACCESS SHARE MODE",
			"ROW SHARE MODE", "ROW EXCLUSIVE MODE",
			"SHARE UPDATE EXCLUSIVE MODE", "SHARE MODE",
			"SHARE ROW EXCLUSIVE MODE",
		"EXCLUSIVE MODE", "ACCESS EXCLUSIVE MODE", NULL};

		COMPLETE_WITH_LIST(lock_modes);
	}

/* NOTIFY */
	else if (pg_strcasecmp(prev_wd, "NOTIFY") == 0)
		COMPLETE_WITH_QUERY("SELECT pg_catalog.quote_ident(channel) FROM pg_catalog.pg_listening_channels() AS channel WHERE substring(pg_catalog.quote_ident(channel),1,%d)='%s'");

/* OPTIONS */
	else if (pg_strcasecmp(prev_wd, "OPTIONS") == 0)
		COMPLETE_WITH_CONST("(");

/* OWNER TO  - complete with available roles */
	else if (pg_strcasecmp(prev2_wd, "OWNER") == 0 &&
			 pg_strcasecmp(prev_wd, "TO") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);

/* ORDER BY */
	else if (pg_strcasecmp(prev3_wd, "FROM") == 0 &&
			 pg_strcasecmp(prev_wd, "ORDER") == 0)
		COMPLETE_WITH_CONST("BY");
	else if (pg_strcasecmp(prev4_wd, "FROM") == 0 &&
			 pg_strcasecmp(prev2_wd, "ORDER") == 0 &&
			 pg_strcasecmp(prev_wd, "BY") == 0)
		COMPLETE_WITH_ATTR(prev3_wd, "");

/* PREPARE xx AS */
	else if (pg_strcasecmp(prev_wd, "AS") == 0 &&
			 pg_strcasecmp(prev3_wd, "PREPARE") == 0)
	{
		static const char *const list_PREPARE[] =
		{"SELECT", "UPDATE", "INSERT", "DELETE", NULL};

		COMPLETE_WITH_LIST(list_PREPARE);
	}

/*
 * PREPARE TRANSACTION is missing on purpose. It's intended for transaction
 * managers, not for manual use in interactive sessions.
 */

/* REASSIGN OWNED BY xxx TO yyy */
	else if (pg_strcasecmp(prev_wd, "REASSIGN") == 0)
		COMPLETE_WITH_CONST("OWNED");
	else if (pg_strcasecmp(prev_wd, "OWNED") == 0 &&
			 pg_strcasecmp(prev2_wd, "REASSIGN") == 0)
		COMPLETE_WITH_CONST("BY");
	else if (pg_strcasecmp(prev_wd, "BY") == 0 &&
			 pg_strcasecmp(prev2_wd, "OWNED") == 0 &&
			 pg_strcasecmp(prev3_wd, "REASSIGN") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (pg_strcasecmp(prev2_wd, "BY") == 0 &&
			 pg_strcasecmp(prev3_wd, "OWNED") == 0 &&
			 pg_strcasecmp(prev4_wd, "REASSIGN") == 0)
		COMPLETE_WITH_CONST("TO");
	else if (pg_strcasecmp(prev_wd, "TO") == 0 &&
			 pg_strcasecmp(prev3_wd, "BY") == 0 &&
			 pg_strcasecmp(prev4_wd, "OWNED") == 0 &&
			 pg_strcasecmp(prev5_wd, "REASSIGN") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);

/* REFRESH MATERIALIZED VIEW */
	else if (pg_strcasecmp(prev_wd, "REFRESH") == 0)
		COMPLETE_WITH_CONST("MATERIALIZED VIEW");
	else if (pg_strcasecmp(prev2_wd, "REFRESH") == 0 &&
			 pg_strcasecmp(prev_wd, "MATERIALIZED") == 0)
		COMPLETE_WITH_CONST("VIEW");
	else if (pg_strcasecmp(prev3_wd, "REFRESH") == 0 &&
			 pg_strcasecmp(prev2_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev_wd, "VIEW") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews,
								   " UNION SELECT 'CONCURRENTLY'");
	else if (pg_strcasecmp(prev4_wd, "REFRESH") == 0 &&
			 pg_strcasecmp(prev3_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev2_wd, "VIEW") == 0 &&
			 pg_strcasecmp(prev_wd, "CONCURRENTLY") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews, NULL);
	else if (pg_strcasecmp(prev4_wd, "REFRESH") == 0 &&
			 pg_strcasecmp(prev3_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev2_wd, "VIEW") == 0)
		COMPLETE_WITH_CONST("WITH");
	else if (pg_strcasecmp(prev5_wd, "REFRESH") == 0 &&
			 pg_strcasecmp(prev4_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev3_wd, "VIEW") == 0 &&
			 pg_strcasecmp(prev2_wd, "CONCURRENTLY") == 0)
		COMPLETE_WITH_CONST("WITH DATA");
	else if (pg_strcasecmp(prev5_wd, "REFRESH") == 0 &&
			 pg_strcasecmp(prev4_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev3_wd, "VIEW") == 0 &&
			 pg_strcasecmp(prev_wd, "WITH") == 0)
	{
		static const char *const list_WITH_DATA[] =
		{"NO DATA", "DATA", NULL};

		COMPLETE_WITH_LIST(list_WITH_DATA);
	}
	else if (pg_strcasecmp(prev6_wd, "REFRESH") == 0 &&
			 pg_strcasecmp(prev5_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev4_wd, "VIEW") == 0 &&
			 pg_strcasecmp(prev3_wd, "CONCURRENTLY") == 0 &&
			 pg_strcasecmp(prev_wd, "WITH") == 0)
		COMPLETE_WITH_CONST("DATA");
	else if (pg_strcasecmp(prev6_wd, "REFRESH") == 0 &&
			 pg_strcasecmp(prev5_wd, "MATERIALIZED") == 0 &&
			 pg_strcasecmp(prev4_wd, "VIEW") == 0 &&
			 pg_strcasecmp(prev2_wd, "WITH") == 0 &&
			 pg_strcasecmp(prev_wd, "NO") == 0)
		COMPLETE_WITH_CONST("DATA");

/* REINDEX */
	else if (pg_strcasecmp(prev_wd, "REINDEX") == 0)
	{
		static const char *const list_REINDEX[] =
		{"TABLE", "INDEX", "SYSTEM", "DATABASE", NULL};

		COMPLETE_WITH_LIST(list_REINDEX);
	}
	else if (pg_strcasecmp(prev2_wd, "REINDEX") == 0)
	{
		if (pg_strcasecmp(prev_wd, "TABLE") == 0)
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm, NULL);
		else if (pg_strcasecmp(prev_wd, "INDEX") == 0)
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes, NULL);
		else if (pg_strcasecmp(prev_wd, "SYSTEM") == 0 ||
				 pg_strcasecmp(prev_wd, "DATABASE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_databases);
	}

/* SECURITY LABEL */
	else if (pg_strcasecmp(prev_wd, "SECURITY") == 0)
		COMPLETE_WITH_CONST("LABEL");
	else if (pg_strcasecmp(prev2_wd, "SECURITY") == 0 &&
			 pg_strcasecmp(prev_wd, "LABEL") == 0)
	{
		static const char *const list_SECURITY_LABEL_preposition[] =
		{"ON", "FOR"};

		COMPLETE_WITH_LIST(list_SECURITY_LABEL_preposition);
	}
	else if (pg_strcasecmp(prev4_wd, "SECURITY") == 0 &&
			 pg_strcasecmp(prev3_wd, "LABEL") == 0 &&
			 pg_strcasecmp(prev2_wd, "FOR") == 0)
		COMPLETE_WITH_CONST("ON");
	else if ((pg_strcasecmp(prev3_wd, "SECURITY") == 0 &&
			  pg_strcasecmp(prev2_wd, "LABEL") == 0 &&
			  pg_strcasecmp(prev_wd, "ON") == 0) ||
			 (pg_strcasecmp(prev5_wd, "SECURITY") == 0 &&
			  pg_strcasecmp(prev4_wd, "LABEL") == 0 &&
			  pg_strcasecmp(prev3_wd, "FOR") == 0 &&
			  pg_strcasecmp(prev_wd, "ON") == 0))
	{
		static const char *const list_SECURITY_LABEL[] =
		{"LANGUAGE", "SCHEMA", "SEQUENCE", "TABLE", "TYPE", "VIEW",
			"MATERIALIZED VIEW", "COLUMN", "AGGREGATE", "FUNCTION", "DOMAIN",
		"LARGE OBJECT", NULL};

		COMPLETE_WITH_LIST(list_SECURITY_LABEL);
	}
	else if (pg_strcasecmp(prev5_wd, "SECURITY") == 0 &&
			 pg_strcasecmp(prev4_wd, "LABEL") == 0 &&
			 pg_strcasecmp(prev3_wd, "ON") == 0)
		COMPLETE_WITH_CONST("IS");

/* SELECT */
	/* naah . . . */

/* SET, RESET, SHOW */
	/* Complete with a variable name */
	else if ((pg_strcasecmp(prev_wd, "SET") == 0 &&
			  pg_strcasecmp(prev3_wd, "UPDATE") != 0) ||
			 pg_strcasecmp(prev_wd, "RESET") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_set_vars);
	else if (pg_strcasecmp(prev_wd, "SHOW") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_show_vars);
	/* Complete "SET TRANSACTION" */
	else if ((pg_strcasecmp(prev2_wd, "SET") == 0 &&
			  pg_strcasecmp(prev_wd, "TRANSACTION") == 0)
			 || (pg_strcasecmp(prev2_wd, "START") == 0
				 && pg_strcasecmp(prev_wd, "TRANSACTION") == 0)
			 || (pg_strcasecmp(prev2_wd, "BEGIN") == 0
				 && pg_strcasecmp(prev_wd, "WORK") == 0)
			 || (pg_strcasecmp(prev2_wd, "BEGIN") == 0
				 && pg_strcasecmp(prev_wd, "TRANSACTION") == 0)
			 || (pg_strcasecmp(prev4_wd, "SESSION") == 0
				 && pg_strcasecmp(prev3_wd, "CHARACTERISTICS") == 0
				 && pg_strcasecmp(prev2_wd, "AS") == 0
				 && pg_strcasecmp(prev_wd, "TRANSACTION") == 0))
	{
		static const char *const my_list[] =
		{"ISOLATION LEVEL", "READ", NULL};

		COMPLETE_WITH_LIST(my_list);
	}
	else if ((pg_strcasecmp(prev3_wd, "SET") == 0
			  || pg_strcasecmp(prev3_wd, "BEGIN") == 0
			  || pg_strcasecmp(prev3_wd, "START") == 0
			  || (pg_strcasecmp(prev4_wd, "CHARACTERISTICS") == 0
				  && pg_strcasecmp(prev3_wd, "AS") == 0))
			 && (pg_strcasecmp(prev2_wd, "TRANSACTION") == 0
				 || pg_strcasecmp(prev2_wd, "WORK") == 0)
			 && pg_strcasecmp(prev_wd, "ISOLATION") == 0)
		COMPLETE_WITH_CONST("LEVEL");
	else if ((pg_strcasecmp(prev4_wd, "SET") == 0
			  || pg_strcasecmp(prev4_wd, "BEGIN") == 0
			  || pg_strcasecmp(prev4_wd, "START") == 0
			  || pg_strcasecmp(prev4_wd, "AS") == 0)
			 && (pg_strcasecmp(prev3_wd, "TRANSACTION") == 0
				 || pg_strcasecmp(prev3_wd, "WORK") == 0)
			 && pg_strcasecmp(prev2_wd, "ISOLATION") == 0
			 && pg_strcasecmp(prev_wd, "LEVEL") == 0)
	{
		static const char *const my_list[] =
		{"READ", "REPEATABLE", "SERIALIZABLE", NULL};

		COMPLETE_WITH_LIST(my_list);
	}
	else if ((pg_strcasecmp(prev4_wd, "TRANSACTION") == 0 ||
			  pg_strcasecmp(prev4_wd, "WORK") == 0) &&
			 pg_strcasecmp(prev3_wd, "ISOLATION") == 0 &&
			 pg_strcasecmp(prev2_wd, "LEVEL") == 0 &&
			 pg_strcasecmp(prev_wd, "READ") == 0)
	{
		static const char *const my_list[] =
		{"UNCOMMITTED", "COMMITTED", NULL};

		COMPLETE_WITH_LIST(my_list);
	}
	else if ((pg_strcasecmp(prev4_wd, "TRANSACTION") == 0 ||
			  pg_strcasecmp(prev4_wd, "WORK") == 0) &&
			 pg_strcasecmp(prev3_wd, "ISOLATION") == 0 &&
			 pg_strcasecmp(prev2_wd, "LEVEL") == 0 &&
			 pg_strcasecmp(prev_wd, "REPEATABLE") == 0)
		COMPLETE_WITH_CONST("READ");
	else if ((pg_strcasecmp(prev3_wd, "SET") == 0 ||
			  pg_strcasecmp(prev3_wd, "BEGIN") == 0 ||
			  pg_strcasecmp(prev3_wd, "START") == 0 ||
			  pg_strcasecmp(prev3_wd, "AS") == 0) &&
			 (pg_strcasecmp(prev2_wd, "TRANSACTION") == 0 ||
			  pg_strcasecmp(prev2_wd, "WORK") == 0) &&
			 pg_strcasecmp(prev_wd, "READ") == 0)
	{
		static const char *const my_list[] =
		{"ONLY", "WRITE", NULL};

		COMPLETE_WITH_LIST(my_list);
	}
	/* SET CONSTRAINTS */
	else if (pg_strcasecmp(prev2_wd, "SET") == 0 &&
			 pg_strcasecmp(prev_wd, "CONSTRAINTS") == 0)
	{
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_constraints_with_schema, "UNION SELECT 'ALL'");
	}
	/* Complete SET CONSTRAINTS <foo> with DEFERRED|IMMEDIATE */
	else if (pg_strcasecmp(prev3_wd, "SET") == 0 &&
			 pg_strcasecmp(prev2_wd, "CONSTRAINTS") == 0)
	{
		static const char *const constraint_list[] =
		{"DEFERRED", "IMMEDIATE", NULL};

		COMPLETE_WITH_LIST(constraint_list);
	}
	/* Complete SET ROLE */
	else if (pg_strcasecmp(prev2_wd, "SET") == 0 &&
			 pg_strcasecmp(prev_wd, "ROLE") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	/* Complete SET SESSION with AUTHORIZATION or CHARACTERISTICS... */
	else if (pg_strcasecmp(prev2_wd, "SET") == 0 &&
			 pg_strcasecmp(prev_wd, "SESSION") == 0)
	{
		static const char *const my_list[] =
		{"AUTHORIZATION", "CHARACTERISTICS AS TRANSACTION", NULL};

		COMPLETE_WITH_LIST(my_list);
	}
	/* Complete SET SESSION AUTHORIZATION with username */
	else if (pg_strcasecmp(prev3_wd, "SET") == 0
			 && pg_strcasecmp(prev2_wd, "SESSION") == 0
			 && pg_strcasecmp(prev_wd, "AUTHORIZATION") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles " UNION SELECT 'DEFAULT'");
	/* Complete RESET SESSION with AUTHORIZATION */
	else if (pg_strcasecmp(prev2_wd, "RESET") == 0 &&
			 pg_strcasecmp(prev_wd, "SESSION") == 0)
		COMPLETE_WITH_CONST("AUTHORIZATION");
	/* Complete SET <var> with "TO" */
	else if (pg_strcasecmp(prev2_wd, "SET") == 0 &&
			 pg_strcasecmp(prev4_wd, "UPDATE") != 0 &&
			 pg_strcasecmp(prev_wd, "TABLESPACE") != 0 &&
			 pg_strcasecmp(prev_wd, "SCHEMA") != 0 &&
			 prev_wd[strlen(prev_wd) - 1] != ')' &&
			 prev_wd[strlen(prev_wd) - 1] != '=' &&
			 pg_strcasecmp(prev4_wd, "DOMAIN") != 0)
		COMPLETE_WITH_CONST("TO");
	/* Suggest possible variable values */
	else if (pg_strcasecmp(prev3_wd, "SET") == 0 &&
			 (pg_strcasecmp(prev_wd, "TO") == 0 || strcmp(prev_wd, "=") == 0))
	{
		if (pg_strcasecmp(prev2_wd, "DateStyle") == 0)
		{
			static const char *const my_list[] =
			{"ISO", "SQL", "Postgres", "German",
				"YMD", "DMY", "MDY",
				"US", "European", "NonEuropean",
			"DEFAULT", NULL};

			COMPLETE_WITH_LIST(my_list);
		}
		else if (pg_strcasecmp(prev2_wd, "IntervalStyle") == 0)
		{
			static const char *const my_list[] =
			{"postgres", "postgres_verbose", "sql_standard", "iso_8601", NULL};

			COMPLETE_WITH_LIST(my_list);
		}
		else if (pg_strcasecmp(prev2_wd, "GEQO") == 0)
		{
			static const char *const my_list[] =
			{"ON", "OFF", "DEFAULT", NULL};

			COMPLETE_WITH_LIST(my_list);
		}
		else
		{
			static const char *const my_list[] =
			{"DEFAULT", NULL};

			COMPLETE_WITH_LIST(my_list);
		}
	}

/* START TRANSACTION */
	else if (pg_strcasecmp(prev_wd, "START") == 0)
		COMPLETE_WITH_CONST("TRANSACTION");

/* TABLE, but not TABLE embedded in other commands */
	else if (pg_strcasecmp(prev_wd, "TABLE") == 0 &&
			 prev2_wd[0] == '\0')
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_relations, NULL);

/* TRUNCATE */
	else if (pg_strcasecmp(prev_wd, "TRUNCATE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);

/* UNLISTEN */
	else if (pg_strcasecmp(prev_wd, "UNLISTEN") == 0)
		COMPLETE_WITH_QUERY("SELECT pg_catalog.quote_ident(channel) FROM pg_catalog.pg_listening_channels() AS channel WHERE substring(pg_catalog.quote_ident(channel),1,%d)='%s' UNION SELECT '*'");

/* UPDATE */
	/* If prev. word is UPDATE suggest a list of tables */
	else if (pg_strcasecmp(prev_wd, "UPDATE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_updatables, NULL);
	/* Complete UPDATE <table> with "SET" */
	else if (pg_strcasecmp(prev2_wd, "UPDATE") == 0)
		COMPLETE_WITH_CONST("SET");

	/*
	 * If the previous word is SET (and it wasn't caught above as the _first_
	 * word) the word before it was (hopefully) a table name and we'll now
	 * make a list of attributes.
	 */
	else if (pg_strcasecmp(prev_wd, "SET") == 0)
		COMPLETE_WITH_ATTR(prev2_wd, "");

/* UPDATE xx SET yy = */
	else if (pg_strcasecmp(prev2_wd, "SET") == 0 &&
			 pg_strcasecmp(prev4_wd, "UPDATE") == 0)
		COMPLETE_WITH_CONST("=");

/* USER MAPPING */
	else if ((pg_strcasecmp(prev3_wd, "ALTER") == 0 ||
			  pg_strcasecmp(prev3_wd, "CREATE") == 0 ||
			  pg_strcasecmp(prev3_wd, "DROP") == 0) &&
			 pg_strcasecmp(prev2_wd, "USER") == 0 &&
			 pg_strcasecmp(prev_wd, "MAPPING") == 0)
		COMPLETE_WITH_CONST("FOR");
	else if (pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev3_wd, "USER") == 0 &&
			 pg_strcasecmp(prev2_wd, "MAPPING") == 0 &&
			 pg_strcasecmp(prev_wd, "FOR") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles
							" UNION SELECT 'CURRENT_USER'"
							" UNION SELECT 'PUBLIC'"
							" UNION SELECT 'USER'");
	else if ((pg_strcasecmp(prev4_wd, "ALTER") == 0 ||
			  pg_strcasecmp(prev4_wd, "DROP") == 0) &&
			 pg_strcasecmp(prev3_wd, "USER") == 0 &&
			 pg_strcasecmp(prev2_wd, "MAPPING") == 0 &&
			 pg_strcasecmp(prev_wd, "FOR") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_user_mappings);
	else if ((pg_strcasecmp(prev5_wd, "CREATE") == 0 ||
			  pg_strcasecmp(prev5_wd, "ALTER") == 0 ||
			  pg_strcasecmp(prev5_wd, "DROP") == 0) &&
			 pg_strcasecmp(prev4_wd, "USER") == 0 &&
			 pg_strcasecmp(prev3_wd, "MAPPING") == 0 &&
			 pg_strcasecmp(prev2_wd, "FOR") == 0)
		COMPLETE_WITH_CONST("SERVER");

/*
 * VACUUM [ FULL | FREEZE ] [ VERBOSE ] [ table ]
 * VACUUM [ FULL | FREEZE ] [ VERBOSE ] ANALYZE [ table [ (column [, ...] ) ] ]
 */
	else if (pg_strcasecmp(prev_wd, "VACUUM") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'FULL'"
								   " UNION SELECT 'FREEZE'"
								   " UNION SELECT 'ANALYZE'"
								   " UNION SELECT 'VERBOSE'");
	else if (pg_strcasecmp(prev2_wd, "VACUUM") == 0 &&
			 (pg_strcasecmp(prev_wd, "FULL") == 0 ||
			  pg_strcasecmp(prev_wd, "FREEZE") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'ANALYZE'"
								   " UNION SELECT 'VERBOSE'");
	else if (pg_strcasecmp(prev3_wd, "VACUUM") == 0 &&
			 pg_strcasecmp(prev_wd, "ANALYZE") == 0 &&
			 (pg_strcasecmp(prev2_wd, "FULL") == 0 ||
			  pg_strcasecmp(prev2_wd, "FREEZE") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'VERBOSE'");
	else if (pg_strcasecmp(prev3_wd, "VACUUM") == 0 &&
			 pg_strcasecmp(prev_wd, "VERBOSE") == 0 &&
			 (pg_strcasecmp(prev2_wd, "FULL") == 0 ||
			  pg_strcasecmp(prev2_wd, "FREEZE") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'ANALYZE'");
	else if (pg_strcasecmp(prev2_wd, "VACUUM") == 0 &&
			 pg_strcasecmp(prev_wd, "VERBOSE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'ANALYZE'");
	else if (pg_strcasecmp(prev2_wd, "VACUUM") == 0 &&
			 pg_strcasecmp(prev_wd, "ANALYZE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm,
								   " UNION SELECT 'VERBOSE'");
	else if ((pg_strcasecmp(prev_wd, "ANALYZE") == 0 &&
			  pg_strcasecmp(prev2_wd, "VERBOSE") == 0) ||
			 (pg_strcasecmp(prev_wd, "VERBOSE") == 0 &&
			  pg_strcasecmp(prev2_wd, "ANALYZE") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tm, NULL);

/* WITH [RECURSIVE] */

	/*
	 * Only match when WITH is the first word, as WITH may appear in many
	 * other contexts.
	 */
	else if (pg_strcasecmp(prev_wd, "WITH") == 0 &&
			 prev2_wd[0] == '\0')
		COMPLETE_WITH_CONST("RECURSIVE");

/* ANALYZE */
	/* If the previous word is ANALYZE, produce list of tables */
	else if (pg_strcasecmp(prev_wd, "ANALYZE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tmf, NULL);

/* WHERE */
	/* Simple case of the word before the where being the table name */
	else if (pg_strcasecmp(prev_wd, "WHERE") == 0)
		COMPLETE_WITH_ATTR(prev2_wd, "");

/* ... FROM ... */
/* TODO: also include SRF ? */
	else if (pg_strcasecmp(prev_wd, "FROM") == 0 &&
			 pg_strcasecmp(prev3_wd, "COPY") != 0 &&
			 pg_strcasecmp(prev3_wd, "\\copy") != 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsvmf, NULL);

/* ... JOIN ... */
	else if (pg_strcasecmp(prev_wd, "JOIN") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsvmf, NULL);

/* Backslash commands */
/* TODO:  \dc \dd \dl */
	else if (strcmp(prev_wd, "\\connect") == 0 || strcmp(prev_wd, "\\c") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_databases);

	else if (strncmp(prev_wd, "\\da", strlen("\\da")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_aggregates, NULL);
	else if (strncmp(prev_wd, "\\db", strlen("\\db")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
	else if (strncmp(prev_wd, "\\dD", strlen("\\dD")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_domains, NULL);
	else if (strncmp(prev_wd, "\\des", strlen("\\des")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_servers);
	else if (strncmp(prev_wd, "\\deu", strlen("\\deu")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_user_mappings);
	else if (strncmp(prev_wd, "\\dew", strlen("\\dew")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_fdws);

	else if (strncmp(prev_wd, "\\df", strlen("\\df")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions, NULL);
	else if (strncmp(prev_wd, "\\dFd", strlen("\\dFd")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_dictionaries);
	else if (strncmp(prev_wd, "\\dFp", strlen("\\dFp")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_parsers);
	else if (strncmp(prev_wd, "\\dFt", strlen("\\dFt")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_templates);
	/* must be at end of \dF */
	else if (strncmp(prev_wd, "\\dF", strlen("\\dF")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_configurations);

	else if (strncmp(prev_wd, "\\di", strlen("\\di")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes, NULL);
	else if (strncmp(prev_wd, "\\dL", strlen("\\dL")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_languages);
	else if (strncmp(prev_wd, "\\dn", strlen("\\dn")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
	else if (strncmp(prev_wd, "\\dp", strlen("\\dp")) == 0
			 || strncmp(prev_wd, "\\z", strlen("\\z")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsvmf, NULL);
	else if (strncmp(prev_wd, "\\ds", strlen("\\ds")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_sequences, NULL);
	else if (strncmp(prev_wd, "\\dt", strlen("\\dt")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
	else if (strncmp(prev_wd, "\\dT", strlen("\\dT")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes, NULL);
	else if (strncmp(prev_wd, "\\du", strlen("\\du")) == 0
			 || (strncmp(prev_wd, "\\dg", strlen("\\dg")) == 0))
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (strncmp(prev_wd, "\\dv", strlen("\\dv")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views, NULL);
	else if (strncmp(prev_wd, "\\dx", strlen("\\dx")) == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_extensions);
	else if (strncmp(prev_wd, "\\dm", strlen("\\dm")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_matviews, NULL);

	/* must be at end of \d list */
	else if (strncmp(prev_wd, "\\d", strlen("\\d")) == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_relations, NULL);

	else if (strcmp(prev_wd, "\\ef") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions, NULL);

	else if (strcmp(prev_wd, "\\encoding") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_encodings);
	else if (strcmp(prev_wd, "\\h") == 0 || strcmp(prev_wd, "\\help") == 0)
		COMPLETE_WITH_LIST(sql_commands);
	else if (strcmp(prev_wd, "\\password") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (strcmp(prev_wd, "\\pset") == 0)
	{
		static const char *const my_list[] =
		{"border", "columns", "expanded", "fieldsep", "fieldsep_zero",
			"footer", "format", "linestyle", "null", "numericlocale",
			"pager", "recordsep", "recordsep_zero", "tableattr", "title",
		"tuples_only", NULL};

		COMPLETE_WITH_LIST_CS(my_list);
	}
	else if (strcmp(prev2_wd, "\\pset") == 0)
	{
		if (strcmp(prev_wd, "format") == 0)
		{
			static const char *const my_list[] =
			{"unaligned", "aligned", "wrapped", "html", "latex",
			"troff-ms", NULL};

			COMPLETE_WITH_LIST_CS(my_list);
		}
		else if (strcmp(prev_wd, "linestyle") == 0)
		{
			static const char *const my_list[] =
			{"ascii", "old-ascii", "unicode", NULL};

			COMPLETE_WITH_LIST_CS(my_list);
		}
	}
	else if (strcmp(prev_wd, "\\set") == 0)
	{
		matches = complete_from_variables(text, "", "");
	}
	else if (strcmp(prev_wd, "\\sf") == 0 || strcmp(prev_wd, "\\sf+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions, NULL);
	else if (strcmp(prev_wd, "\\cd") == 0 ||
			 strcmp(prev_wd, "\\e") == 0 || strcmp(prev_wd, "\\edit") == 0 ||
			 strcmp(prev_wd, "\\g") == 0 ||
		  strcmp(prev_wd, "\\i") == 0 || strcmp(prev_wd, "\\include") == 0 ||
			 strcmp(prev_wd, "\\ir") == 0 || strcmp(prev_wd, "\\include_relative") == 0 ||
			 strcmp(prev_wd, "\\o") == 0 || strcmp(prev_wd, "\\out") == 0 ||
			 strcmp(prev_wd, "\\s") == 0 ||
			 strcmp(prev_wd, "\\w") == 0 || strcmp(prev_wd, "\\write") == 0 ||
			 strcmp(prev_wd, "\\lo_import") == 0
		)
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
	{
		int			i;

		for (i = 0; i < lengthof(previous_words); i++)
			free(previous_words[i]);
	}

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
		e_text = pg_malloc(string_length * 2 + 1);
		PQescapeString(e_text, text, string_length);

		if (completion_info_charp)
		{
			size_t		charp_len;

			charp_len = strlen(completion_info_charp);
			e_info_charp = pg_malloc(charp_len * 2 + 1);
			PQescapeString(e_info_charp, completion_info_charp,
						   charp_len);
		}
		else
			e_info_charp = NULL;

		if (completion_info_charp2)
		{
			size_t		charp_len;

			charp_len = strlen(completion_info_charp2);
			e_info_charp2 = pg_malloc(charp_len * 2 + 1);
			PQescapeString(e_info_charp2, completion_info_charp2,
						   charp_len);
		}
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
 * This function supports completion with the name of a psql variable.
 * The variable names can be prefixed and suffixed with additional text
 * to support quoting usages.
 */
static char **
complete_from_variables(const char *text, const char *prefix, const char *suffix)
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
		if (nvars >= maxvars)
		{
			maxvars *= 2;
			varnames = (char **) pg_realloc(varnames,
											(maxvars + 1) * sizeof(char *));
		}

		varnames[nvars++] = psprintf("%s%s%s", prefix, ptr->name, suffix);
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
 * Return the nwords word(s) before point.  Words are returned right to left,
 * that is, previous_words[0] gets the last word before point.
 * If we run out of words, remaining array elements are set to empty strings.
 * Each array element is filled with a malloc'd string.
 */
static void
get_previous_words(int point, char **previous_words, int nwords)
{
	const char *buf = rl_line_buffer;	/* alias */
	int			i;

	/* first we look for a non-word char before the current point */
	for (i = point - 1; i >= 0; i--)
		if (strchr(WORD_BREAKS, buf[i]))
			break;
	point = i;

	while (nwords-- > 0)
	{
		int			start,
					end;
		char	   *s;

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

		/*
		 * If no end found we return an empty string, because there is no word
		 * before the point
		 */
		if (end < 0)
		{
			point = end;
			s = pg_strdup("");
		}
		else
		{
			/*
			 * Otherwise we now look for the start. The start is either the
			 * last character before any word-break character going backwards
			 * from the end, or it's simply character 0. We also handle open
			 * quotes and parentheses.
			 */
			bool		inquotes = false;
			int			parentheses = 0;

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

			point = start - 1;

			/* make a copy of chars from start to end inclusive */
			s = pg_malloc(end - start + 2);
			strlcpy(s, &buf[start], end - start + 2);
		}

		*previous_words++ = s;
	}
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
