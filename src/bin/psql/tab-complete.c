/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/tab-complete.c,v 1.169 2008/01/01 19:45:56 momjian Exp $
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

#ifdef HAVE_RL_FILENAME_COMPLETION_FUNCTION
#define filename_completion_function rl_filename_completion_function
#else
/* missing in some header files */
extern char *filename_completion_function();
#endif

#ifdef HAVE_RL_COMPLETION_MATCHES
#define completion_matches rl_completion_matches
#endif


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
	 * to display.	If catname mentions multiple tables, include the necessary
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
static const SchemaQuery *completion_squery;	/* to pass a SchemaQuery */

/* A couple of macros to ease typing. You can use these to complete the given
   string with
   1) The results from a query you pass it. (Perhaps one of those below?)
   2) The results from a schema query you pass it.
   3) The items from a null-pointer-terminated list.
   4) A string constant
   5) The list of attributes to the given table.
*/
#define COMPLETE_WITH_QUERY(query) \
do { completion_charp = query; matches = completion_matches(text, complete_from_query); } while(0)
#define COMPLETE_WITH_SCHEMA_QUERY(query, addon) \
do { completion_squery = &(query); completion_charp = addon; matches = completion_matches(text, complete_from_schema_query); } while(0)
#define COMPLETE_WITH_LIST(list) \
do { completion_charpp = list; matches = completion_matches(text, complete_from_list); } while(0)
#define COMPLETE_WITH_CONST(string) \
do { completion_charp = string; matches = completion_matches(text, complete_from_const); } while(0)
#define COMPLETE_WITH_ATTR(table, addon) \
do {completion_charp = Query_for_list_of_attributes addon; completion_info_charp = table; matches = completion_matches(text, complete_from_query); } while(0)

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

static const SchemaQuery Query_for_list_of_tisv = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('r', 'i', 'S', 'v')",
	/* viscondition */
	"pg_catalog.pg_table_is_visible(c.oid)",
	/* namespace */
	"c.relnamespace",
	/* result */
	"pg_catalog.quote_ident(c.relname)",
	/* qualresult */
	NULL
};

static const SchemaQuery Query_for_list_of_tsv = {
	/* catname */
	"pg_catalog.pg_class c",
	/* selcondition */
	"c.relkind IN ('r', 'S', 'v')",
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


/*
 * Queries to get lists of names of various kinds of things, possibly
 * restricted to names matching a partially entered name.  In these queries,
 * %s will be replaced by the text entered so far (suitably escaped to
 * become a SQL literal string).  %d will be replaced by the length of the
 * string (in unescaped form).	A second %s, if present, will be replaced
 * by a suitably-escaped version of the string provided in
 * completion_info_charp.
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
"   AND pg_catalog.quote_ident(relname)='%s' "\
"   AND pg_catalog.pg_table_is_visible(c.oid)"

#define Query_for_list_of_template_databases \
"SELECT pg_catalog.quote_ident(datname) FROM pg_catalog.pg_database "\
" WHERE substring(pg_catalog.quote_ident(datname),1,%d)='%s' and datistemplate IS TRUE"

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
"  FROM pg_language "\
" WHERE lanname != 'internal' "\
"   AND substring(pg_catalog.quote_ident(lanname),1,%d)='%s' "

#define Query_for_list_of_schemas \
"SELECT pg_catalog.quote_ident(nspname) FROM pg_catalog.pg_namespace "\
" WHERE substring(pg_catalog.quote_ident(nspname),1,%d)='%s'"

#define Query_for_list_of_set_vars \
"SELECT name FROM "\
" (SELECT pg_catalog.lower(name) AS name FROM pg_catalog.pg_settings "\
"  WHERE context IN ('user', 'superuser') "\
"  UNION ALL SELECT 'constraints' "\
"  UNION ALL SELECT 'transaction' "\
"  UNION ALL SELECT 'session' "\
"  UNION ALL SELECT 'role' "\
"  UNION ALL SELECT 'all') ss "\
" WHERE substring(name,1,%d)='%s'"

#define Query_for_list_of_show_vars \
"SELECT name FROM "\
" (SELECT pg_catalog.lower(name) AS name FROM pg_catalog.pg_settings "\
"  UNION ALL SELECT 'session authorization' "\
"  UNION ALL SELECT 'all') ss "\
" WHERE substring(name,1,%d)='%s'"

/*
 * Note: As of Pg 8.2, we no longer use relkind 's', but we keep it here
 * for compatibility with older servers
 */
#define Query_for_list_of_system_relations \
"SELECT pg_catalog.quote_ident(relname) "\
"  FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
" WHERE c.relkind IN ('r', 'v', 's', 'S') "\
"   AND substring(pg_catalog.quote_ident(relname),1,%d)='%s' "\
"   AND c.relnamespace = n.oid "\
"   AND n.nspname = 'pg_catalog'"

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

/*
 * This is a list of all "things" in Pgsql, which can show up after CREATE or
 * DROP; and there is also a query to get a list of them.
 */

typedef struct
{
	const char *name;
	const char *query;			/* simple query, or NULL */
	const SchemaQuery *squery;	/* schema query, or NULL */
	const bool	noshow;			/* NULL or true if this word should not show
								 * up after CREATE or DROP */
} pgsql_thing_t;

static const pgsql_thing_t words_after_create[] = {
	{"AGGREGATE", NULL, &Query_for_list_of_aggregates},
	{"CAST", NULL, NULL},		/* Casts have complex structures for names, so
								 * skip it */

	/*
	 * CREATE CONSTRAINT TRIGGER is not supported here because it is designed
	 * to be used only by pg_dump.
	 */
	{"CONFIGURATION", Query_for_list_of_ts_configurations, NULL, true},
	{"CONVERSION", "SELECT pg_catalog.quote_ident(conname) FROM pg_catalog.pg_conversion WHERE substring(pg_catalog.quote_ident(conname),1,%d)='%s'"},
	{"DATABASE", Query_for_list_of_databases},
	{"DICTIONARY", Query_for_list_of_ts_dictionaries, NULL, true},
	{"DOMAIN", NULL, &Query_for_list_of_domains},
	{"FUNCTION", NULL, &Query_for_list_of_functions},
	{"GROUP", Query_for_list_of_roles},
	{"LANGUAGE", Query_for_list_of_languages},
	{"INDEX", NULL, &Query_for_list_of_indexes},
	{"OPERATOR", NULL, NULL},	/* Querying for this is probably not such a
								 * good idea. */
	{"PARSER", Query_for_list_of_ts_parsers, NULL, true},
	{"ROLE", Query_for_list_of_roles},
	{"RULE", "SELECT pg_catalog.quote_ident(rulename) FROM pg_catalog.pg_rules WHERE substring(pg_catalog.quote_ident(rulename),1,%d)='%s'"},
	{"SCHEMA", Query_for_list_of_schemas},
	{"SEQUENCE", NULL, &Query_for_list_of_sequences},
	{"TABLE", NULL, &Query_for_list_of_tables},
	{"TABLESPACE", Query_for_list_of_tablespaces},
	{"TEMP", NULL, NULL},		/* for CREATE TEMP TABLE ... */
	{"TEMPLATE", Query_for_list_of_ts_templates, NULL, true},
	{"TEXT SEARCH", NULL, NULL},
	{"TRIGGER", "SELECT pg_catalog.quote_ident(tgname) FROM pg_catalog.pg_trigger WHERE substring(pg_catalog.quote_ident(tgname),1,%d)='%s'"},
	{"TYPE", NULL, &Query_for_list_of_datatypes},
	{"UNIQUE", NULL, NULL},		/* for CREATE UNIQUE INDEX ... */
	{"USER", Query_for_list_of_roles},
	{"VIEW", NULL, &Query_for_list_of_views},
	{NULL, NULL, NULL, false}	/* end of list */
};


/* Forward declaration of functions */
static char **psql_completion(char *text, int start, int end);
static char *create_command_generator(const char *text, int state);
static char *drop_command_generator(const char *text, int state);
static char *complete_from_query(const char *text, int state);
static char *complete_from_schema_query(const char *text, int state);
static char *_complete_from_query(int is_schema_query,
					 const char *text, int state);
static char *complete_from_const(const char *text, int state);
static char *complete_from_list(const char *text, int state);

static PGresult *exec_query(const char *query);

static char *previous_word(int point, int skip);

static int	find_open_parenthesis(int end);

#if 0
static char *quote_file_name(char *text, int match_type, char *quote_pointer);
static char *dequote_file_name(char *text, char quote_char);
#endif


/* Initialize the readline library for our purposes. */
void
initialize_readline(void)
{
	rl_readline_name = (char *) pset.progname;
	rl_attempted_completion_function = (void *) psql_completion;

	rl_basic_word_break_characters = "\t\n@$><=;|&{( ";

	completion_max_records = 1000;

	/*
	 * There is a variable rl_completion_query_items for this but apparently
	 * it's not defined everywhere.
	 */
}


/* The completion function. Acc. to readline spec this gets passed the text
   entered to far and its start and end in the readline buffer. The return value
   is some partially obscure list format that can be generated by the readline
   libraries completion_matches() function, so we don't have to worry about it.
*/
static char **
psql_completion(char *text, int start, int end)
{
	/* This is the variable we'll return. */
	char	  **matches = NULL;

	/* These are going to contain some scannage of the input line. */
	char	   *prev_wd,
			   *prev2_wd,
			   *prev3_wd,
			   *prev4_wd,
			   *prev5_wd;

	static const char *const sql_commands[] = {
		"ABORT", "ALTER", "ANALYZE", "BEGIN", "CHECKPOINT", "CLOSE", "CLUSTER",
		"COMMENT", "COMMIT", "COPY", "CREATE", "DEALLOCATE", "DECLARE",
		"DELETE FROM", "DISCARD", "DROP", "END", "EXECUTE", "EXPLAIN", "FETCH",
		"GRANT", "INSERT", "LISTEN", "LOAD", "LOCK", "MOVE", "NOTIFY", "PREPARE",
		"REASSIGN", "REINDEX", "RELEASE", "RESET", "REVOKE", "ROLLBACK",
		"SAVEPOINT", "SELECT", "SET", "SHOW", "START", "TRUNCATE", "UNLISTEN",
		"UPDATE", "VACUUM", "VALUES", NULL
	};

	static const char *const backslash_commands[] = {
		"\\a", "\\connect", "\\C", "\\cd", "\\copy", "\\copyright",
		"\\d", "\\da", "\\db", "\\dc", "\\dC", "\\dd", "\\dD", "\\df",
		"\\dF", "\\dFd", "\\dFp", "\\dFt", "\\dg", "\\di", "\\dl",
		"\\dn", "\\do", "\\dp", "\\ds", "\\dS", "\\dt", "\\dT", "\\dv", "\\du",
		"\\e", "\\echo", "\\encoding",
		"\\f", "\\g", "\\h", "\\help", "\\H", "\\i", "\\l",
		"\\lo_import", "\\lo_export", "\\lo_list", "\\lo_unlink",
		"\\o", "\\p", "\\password", "\\prompt", "\\pset", "\\q", "\\qecho", "\\r",
		"\\set", "\\t", "\\T",
		"\\timing", "\\unset", "\\x", "\\w", "\\z", "\\!", NULL
	};

	(void) end;					/* not used */

#ifdef HAVE_RL_COMPLETION_APPEND_CHARACTER
	rl_completion_append_character = ' ';
#endif

	/* Clear a few things. */
	completion_charp = NULL;
	completion_charpp = NULL;
	completion_info_charp = NULL;

	/*
	 * Scan the input line before our current position for the last four
	 * words. According to those we'll make some smart decisions on what the
	 * user is probably intending to type. TODO: Use strtokx() to do this.
	 */
	prev_wd = previous_word(start, 0);
	prev2_wd = previous_word(start, 1);
	prev3_wd = previous_word(start, 2);
	prev4_wd = previous_word(start, 3);
	prev5_wd = previous_word(start, 4);

	/* If a backslash command was started, continue */
	if (text[0] == '\\')
		COMPLETE_WITH_LIST(backslash_commands);

	/* If no previous word, suggest one of the basic sql commands */
	else if (!prev_wd)
		COMPLETE_WITH_LIST(sql_commands);

/* CREATE */
	/* complete with something you can create */
	else if (pg_strcasecmp(prev_wd, "CREATE") == 0)
		matches = completion_matches(text, create_command_generator);

/* DROP, except ALTER (TABLE|DOMAIN|GROUP) sth DROP */
	/* complete with something you can drop */
	else if (pg_strcasecmp(prev_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev3_wd, "TABLE") != 0 &&
			 pg_strcasecmp(prev3_wd, "DOMAIN") != 0 &&
			 pg_strcasecmp(prev3_wd, "GROUP") != 0)
		matches = completion_matches(text, drop_command_generator);

/* ALTER */

	/*
	 * complete with what you can alter (TABLE, GROUP, USER, ...) unless we're
	 * in ALTER TABLE sth ALTER
	 */
	else if (pg_strcasecmp(prev_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TABLE") != 0)
	{
		static const char *const list_ALTER[] =
		{"AGGREGATE", "CONVERSION", "DATABASE", "DOMAIN", "FUNCTION",
			"GROUP", "INDEX", "LANGUAGE", "OPERATOR", "ROLE", "SCHEMA", "SEQUENCE", "TABLE",
		"TABLESPACE", "TEXT SEARCH", "TRIGGER", "TYPE", "USER", "VIEW", NULL};

		COMPLETE_WITH_LIST(list_ALTER);
	}
	/* ALTER AGGREGATE,FUNCTION <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 (pg_strcasecmp(prev2_wd, "AGGREGATE") == 0 ||
			  pg_strcasecmp(prev2_wd, "FUNCTION") == 0))
	{
		static const char *const list_ALTERAGG[] =
		{"OWNER TO", "RENAME TO", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERAGG);
	}

	/* ALTER CONVERSION,SCHEMA <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 (pg_strcasecmp(prev2_wd, "CONVERSION") == 0 ||
			  pg_strcasecmp(prev2_wd, "SCHEMA") == 0))
	{
		static const char *const list_ALTERGEN[] =
		{"OWNER TO", "RENAME TO", NULL};

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

	/* ALTER INDEX <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "INDEX") == 0)
	{
		static const char *const list_ALTERINDEX[] =
		{"SET TABLESPACE", "OWNER TO", "RENAME TO", "SET", "RESET", NULL};

		COMPLETE_WITH_LIST(list_ALTERINDEX);
	}

	/* ALTER LANGUAGE <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "LANGUAGE") == 0)
	{
		static const char *const list_ALTERLANGUAGE[] =
		{"OWNER TO", "RENAME TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERLANGUAGE);
	}

	/* ALTER USER,ROLE <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 (pg_strcasecmp(prev2_wd, "USER") == 0 ||
			  pg_strcasecmp(prev2_wd, "ROLE") == 0))
	{
		static const char *const list_ALTERUSER[] =
		{"ENCRYPTED", "UNENCRYPTED", "CREATEDB", "NOCREATEDB", "CREATEUSER",
			"NOCREATEUSER", "CREATEROLE", "NOCREATEROLE", "INHERIT", "NOINHERIT",
			"LOGIN", "NOLOGIN", "CONNECTION LIMIT", "VALID UNTIL", "RENAME TO",
		"SUPERUSER", "NOSUPERUSER", "SET", "RESET", NULL};

		COMPLETE_WITH_LIST(list_ALTERUSER);
	}

	/* complete ALTER USER,ROLE <name> ENCRYPTED,UNENCRYPTED with PASSWORD */
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 (pg_strcasecmp(prev3_wd, "ROLE") == 0 || pg_strcasecmp(prev3_wd, "USER") == 0) &&
			 (pg_strcasecmp(prev_wd, "ENCRYPTED") == 0 || pg_strcasecmp(prev_wd, "UNENCRYPTED") == 0))
	{
		COMPLETE_WITH_CONST("PASSWORD");
	}
	/* ALTER DOMAIN <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "DOMAIN") == 0)
	{
		static const char *const list_ALTERDOMAIN[] =
		{"ADD", "DROP", "OWNER TO", "SET", NULL};

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
		"SET SCHEMA", "OWNED BY", "RENAME TO", NULL};

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
	/* ALTER VIEW <name> */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "VIEW") == 0)
	{
		static const char *const list_ALTERVIEW[] = {"RENAME TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERVIEW);
	}
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
	else if (pg_strcasecmp(prev4_wd, "TRIGGER") == 0 &&
			 pg_strcasecmp(prev2_wd, "ON") == 0)
		COMPLETE_WITH_CONST("RENAME TO");

	/*
	 * If we detect ALTER TABLE <name>, suggest either ADD, DROP, ALTER,
	 * RENAME, CLUSTER ON or OWNER
	 */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "TABLE") == 0)
	{
		static const char *const list_ALTER2[] =
		{"ADD", "ALTER", "CLUSTER ON", "DISABLE", "DROP", "ENABLE", "INHERIT",
		"NO INHERIT", "RENAME", "RESET", "OWNER TO", "SET", NULL};

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
	else if (pg_strcasecmp(prev4_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev_wd, "DISABLE") == 0)
	{
		static const char *const list_ALTERDISABLE[] =
		{"RULE", "TRIGGER", NULL};

		COMPLETE_WITH_LIST(list_ALTERDISABLE);
	}

	/* If we have TABLE <sth> ALTER|RENAME, provide list of columns */
	else if (pg_strcasecmp(prev3_wd, "TABLE") == 0 &&
			 (pg_strcasecmp(prev_wd, "ALTER") == 0 ||
			  pg_strcasecmp(prev_wd, "RENAME") == 0))
		COMPLETE_WITH_ATTR(prev2_wd, " UNION SELECT 'COLUMN'");

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
			 pg_strcasecmp(prev_wd, "TO") != 0)
		COMPLETE_WITH_CONST("TO");

	/* ALTER TABLE xxx RENAME COLUMN yyy */
	else if (pg_strcasecmp(prev5_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev3_wd, "RENAME") == 0 &&
			 pg_strcasecmp(prev2_wd, "COLUMN") == 0 &&
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
	/* If we have TABLE <sth> DROP COLUMN, provide list of columns */
	else if (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			 pg_strcasecmp(prev2_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev_wd, "COLUMN") == 0)
		COMPLETE_WITH_ATTR(prev3_wd, "");
	/* ALTER TABLE ALTER [COLUMN] <foo> */
	else if ((pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			  pg_strcasecmp(prev2_wd, "COLUMN") == 0) ||
			 (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
			  pg_strcasecmp(prev2_wd, "ALTER") == 0))
	{
		/* DROP ... does not work well yet */
		static const char *const list_COLUMNALTER[] =
		{"TYPE", "SET DEFAULT", "DROP DEFAULT", "SET NOT NULL",
		"DROP NOT NULL", "SET STATISTICS", "SET STORAGE", NULL};

		COMPLETE_WITH_LIST(list_COLUMNALTER);
	}
	else if (pg_strcasecmp(prev3_wd, "TABLE") == 0)
		COMPLETE_WITH_CONST("ON");
	else if (pg_strcasecmp(prev4_wd, "TABLE") == 0 &&
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
		{"WITHOUT", "TABLESPACE", "SCHEMA", NULL};

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
	/* we have ALTER TABLESPACE, so suggest RENAME TO, OWNER TO */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "TABLESPACE") == 0)
	{
		static const char *const list_ALTERTSPC[] =
		{"RENAME TO", "OWNER TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERTSPC);
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
		COMPLETE_WITH_CONST("RENAME TO");

	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "TEXT") == 0 &&
			 pg_strcasecmp(prev3_wd, "SEARCH") == 0 &&
			 pg_strcasecmp(prev2_wd, "DICTIONARY") == 0)
	{
		static const char *const list_ALTERTEXTSEARCH2[] =
		{"OWNER TO", "RENAME TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERTEXTSEARCH2);
	}

	else if (pg_strcasecmp(prev5_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev4_wd, "TEXT") == 0 &&
			 pg_strcasecmp(prev3_wd, "SEARCH") == 0 &&
			 pg_strcasecmp(prev2_wd, "CONFIGURATION") == 0)
	{
		static const char *const list_ALTERTEXTSEARCH3[] =
		{"ADD MAPPING FOR", "ALTER MAPPING", "DROP MAPPING FOR", "OWNER TO", "RENAME TO", NULL};

		COMPLETE_WITH_LIST(list_ALTERTEXTSEARCH3);
	}

	/* complete ALTER TYPE <foo> with OWNER TO, SET SCHEMA */
	else if (pg_strcasecmp(prev3_wd, "ALTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "TYPE") == 0)
	{
		static const char *const list_ALTERTYPE[] =
		{"OWNER TO", "SET SCHEMA", NULL};

		COMPLETE_WITH_LIST(list_ALTERTYPE);
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
	 * If the previous word is CLUSTER and not without produce list of tables
	 */
	else if (pg_strcasecmp(prev_wd, "CLUSTER") == 0 &&
			 pg_strcasecmp(prev2_wd, "WITHOUT") != 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
	/* If we have CLUSTER <sth>, then add "USING" */
	else if (pg_strcasecmp(prev2_wd, "CLUSTER") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") != 0)
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

/* COMMENT */
	else if (pg_strcasecmp(prev_wd, "COMMENT") == 0)
		COMPLETE_WITH_CONST("ON");
	else if (pg_strcasecmp(prev2_wd, "COMMENT") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
	{
		static const char *const list_COMMENT[] =
		{"CAST", "CONVERSION", "DATABASE", "INDEX", "LANGUAGE", "RULE", "SCHEMA",
			"SEQUENCE", "TABLE", "TYPE", "VIEW", "COLUMN", "AGGREGATE", "FUNCTION",
			"OPERATOR", "TRIGGER", "CONSTRAINT", "DOMAIN", "LARGE OBJECT",
		"TABLESPACE", "TEXT SEARCH", "ROLE", NULL};

		COMPLETE_WITH_LIST(list_COMMENT);
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
	else if ((pg_strcasecmp(prev4_wd, "COMMENT") == 0 &&
			  pg_strcasecmp(prev3_wd, "ON") == 0) ||
			 (pg_strcasecmp(prev5_wd, "ON") == 0 &&
			  pg_strcasecmp(prev4_wd, "TEXT") == 0 &&
			  pg_strcasecmp(prev3_wd, "SEARCH") == 0))
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
		matches = completion_matches(text, filename_completion_function);

	/* Handle COPY|BINARY <sth> FROM|TO filename */
	else if ((pg_strcasecmp(prev4_wd, "COPY") == 0 ||
			  pg_strcasecmp(prev4_wd, "\\copy") == 0 ||
			  pg_strcasecmp(prev4_wd, "BINARY") == 0) &&
			 (pg_strcasecmp(prev2_wd, "FROM") == 0 ||
			  pg_strcasecmp(prev2_wd, "TO") == 0))
	{
		static const char *const list_COPY[] =
		{"BINARY", "OIDS", "DELIMITER", "NULL", "CSV", NULL};

		COMPLETE_WITH_LIST(list_COPY);
	}

	/* Handle COPY|BINARY <sth> FROM|TO filename CSV */
	else if (pg_strcasecmp(prev_wd, "CSV") == 0 &&
			 (pg_strcasecmp(prev3_wd, "FROM") == 0 ||
			  pg_strcasecmp(prev3_wd, "TO") == 0))
	{
		static const char *const list_CSV[] =
		{"HEADER", "QUOTE", "ESCAPE", "FORCE QUOTE", NULL};

		COMPLETE_WITH_LIST(list_CSV);
	}

	/* CREATE DATABASE */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev2_wd, "DATABASE") == 0)
	{
		static const char *const list_DATABASE[] =
		{"OWNER", "TEMPLATE", "ENCODING", "TABLESPACE", "CONNECTION LIMIT",
		NULL};

		COMPLETE_WITH_LIST(list_DATABASE);
	}

	else if (pg_strcasecmp(prev4_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev3_wd, "DATABASE") == 0 &&
			 pg_strcasecmp(prev_wd, "TEMPLATE") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_template_databases);

	/* CREATE INDEX */
	/* First off we complete CREATE UNIQUE with "INDEX" */
	else if (pg_strcasecmp(prev2_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev_wd, "UNIQUE") == 0)
		COMPLETE_WITH_CONST("INDEX");
	/* If we have CREATE|UNIQUE INDEX <sth>, then add "ON" */
	else if (pg_strcasecmp(prev2_wd, "INDEX") == 0 &&
			 (pg_strcasecmp(prev3_wd, "CREATE") == 0 ||
			  pg_strcasecmp(prev3_wd, "UNIQUE") == 0))
		COMPLETE_WITH_CONST("ON");
	/* Complete ... INDEX <name> ON with a list of tables  */
	else if (pg_strcasecmp(prev3_wd, "INDEX") == 0 &&
			 pg_strcasecmp(prev_wd, "ON") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);

	/*
	 * Complete INDEX <name> ON <table> with a list of table columns (which
	 * should really be in parens)
	 */
	else if (pg_strcasecmp(prev4_wd, "INDEX") == 0 &&
			 pg_strcasecmp(prev2_wd, "ON") == 0)
	{
		if (find_open_parenthesis(end))
			COMPLETE_WITH_ATTR(prev_wd, "");
		else
			COMPLETE_WITH_CONST("(");
	}
	else if (pg_strcasecmp(prev5_wd, "INDEX") == 0 &&
			 pg_strcasecmp(prev3_wd, "ON") == 0 &&
			 pg_strcasecmp(prev_wd, "(") == 0)
		COMPLETE_WITH_ATTR(prev2_wd, "");
	/* same if you put in USING */
	else if (pg_strcasecmp(prev4_wd, "ON") == 0 &&
			 pg_strcasecmp(prev2_wd, "USING") == 0)
		COMPLETE_WITH_ATTR(prev3_wd, "");
	/* Complete USING with an index method */
	else if (pg_strcasecmp(prev_wd, "USING") == 0)
	{
		static const char *const index_mth[] =
		{"BTREE", "HASH", "GIN", "GIST", NULL};

		COMPLETE_WITH_LIST(index_mth);
	}

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
		{"BEFORE", "AFTER", NULL};

		COMPLETE_WITH_LIST(list_CREATETRIGGER);
	}
	/* complete CREATE TRIGGER <name> BEFORE,AFTER sth with OR,ON */
	else if (pg_strcasecmp(prev5_wd, "CREATE") == 0 &&
			 pg_strcasecmp(prev4_wd, "TRIGGER") == 0 &&
			 (pg_strcasecmp(prev2_wd, "BEFORE") == 0 ||
			  pg_strcasecmp(prev2_wd, "AFTER") == 0))
	{
		static const char *const list_CREATETRIGGER2[] =
		{"ON", "OR", NULL};

		COMPLETE_WITH_LIST(list_CREATETRIGGER2);
	}

/* CREATE ROLE,USER,GROUP */
	else if (pg_strcasecmp(prev3_wd, "CREATE") == 0 &&
			 (pg_strcasecmp(prev2_wd, "ROLE") == 0 ||
			  pg_strcasecmp(prev2_wd, "GROUP") == 0 || pg_strcasecmp(prev2_wd, "USER") == 0))
	{
		static const char *const list_CREATEROLE[] =
		{"ADMIN", "CONNECTION LIMIT", "CREATEDB", "CREATEROLE", "CREATEUSER",
			"ENCRYPTED", "IN", "INHERIT", "LOGIN", "NOINHERIT", "NOLOGIN", "NOCREATEDB",
			"NOCREATEROLE", "NOCREATEUSER", "NOSUPERUSER", "ROLE", "SUPERUSER", "SYSID",
		"UNENCRYPTED", NULL};

		COMPLETE_WITH_LIST(list_CREATEROLE);
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

/* DECLARE */
	else if (pg_strcasecmp(prev2_wd, "DECLARE") == 0)
	{
		static const char *const list_DECLARE[] =
		{"BINARY", "INSENSITIVE", "SCROLL", "NO SCROLL", "CURSOR", NULL};

		COMPLETE_WITH_LIST(list_DECLARE);
	}

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
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
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
		{"ALL", "PLANS", "TEMP", NULL};

		COMPLETE_WITH_LIST(list_DISCARD);
	}

/* DROP (when not the previous word) */
	/* DROP AGGREGATE */
	else if (pg_strcasecmp(prev3_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev2_wd, "AGGREGATE") == 0)
		COMPLETE_WITH_CONST("(");

	/* DROP object with CASCADE / RESTRICT */
	else if ((pg_strcasecmp(prev3_wd, "DROP") == 0 &&
			  (pg_strcasecmp(prev2_wd, "CONVERSION") == 0 ||
			   pg_strcasecmp(prev2_wd, "DOMAIN") == 0 ||
			   pg_strcasecmp(prev2_wd, "FUNCTION") == 0 ||
			   pg_strcasecmp(prev2_wd, "INDEX") == 0 ||
			   pg_strcasecmp(prev2_wd, "LANGUAGE") == 0 ||
			   pg_strcasecmp(prev2_wd, "SCHEMA") == 0 ||
			   pg_strcasecmp(prev2_wd, "SEQUENCE") == 0 ||
			   pg_strcasecmp(prev2_wd, "TABLE") == 0 ||
			   pg_strcasecmp(prev2_wd, "TYPE") == 0 ||
			   pg_strcasecmp(prev2_wd, "VIEW") == 0)) ||
			 (pg_strcasecmp(prev4_wd, "DROP") == 0 &&
			  pg_strcasecmp(prev3_wd, "AGGREGATE") == 0 &&
			  prev_wd[strlen(prev_wd) - 1] == ')') ||
			 (pg_strcasecmp(prev5_wd, "DROP") == 0 &&
			  pg_strcasecmp(prev4_wd, "TEXT") == 0 &&
			  pg_strcasecmp(prev3_wd, "SEARCH") == 0 &&
			  (pg_strcasecmp(prev2_wd, "CONFIGURATION") == 0 ||
			   pg_strcasecmp(prev2_wd, "DICTIONARY") == 0 ||
			   pg_strcasecmp(prev2_wd, "PARSER") == 0 ||
			   pg_strcasecmp(prev2_wd, "TEMPLATE") == 0))
		)
	{
		if ((pg_strcasecmp(prev3_wd, "DROP") == 0) && (pg_strcasecmp(prev2_wd, "FUNCTION") == 0))
		{
			if (find_open_parenthesis(end))
			{
				static const char func_args_query[] = "select pg_catalog.oidvectortypes(proargtypes)||')' from pg_proc where proname='%s'";
				char	   *tmp_buf = malloc(strlen(func_args_query) + strlen(prev_wd));

				sprintf(tmp_buf, func_args_query, prev_wd);
				COMPLETE_WITH_QUERY(tmp_buf);
				free(tmp_buf);
			}
			else
			{
				COMPLETE_WITH_CONST("(");
			}
		}
		else
		{
			static const char *const list_DROPCR[] =
			{"CASCADE", "RESTRICT", NULL};

			COMPLETE_WITH_LIST(list_DROPCR);
		}
	}
	else if (pg_strcasecmp(prev4_wd, "DROP") == 0 &&
			 pg_strcasecmp(prev3_wd, "FUNCTION") == 0 &&
			 pg_strcasecmp(prev_wd, "(") == 0)
	{
		static const char func_args_query[] = "select pg_catalog.oidvectortypes(proargtypes)||')' from pg_proc where proname='%s'";
		char	   *tmp_buf = malloc(strlen(func_args_query) + strlen(prev2_wd));

		sprintf(tmp_buf, func_args_query, prev2_wd);
		COMPLETE_WITH_QUERY(tmp_buf);
		free(tmp_buf);
	}
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
	else if (pg_strcasecmp(prev_wd, "VERBOSE") == 0 &&
			 pg_strcasecmp(prev3_wd, "VACUUM") != 0 &&
			 pg_strcasecmp(prev4_wd, "VACUUM") != 0 &&
			 (pg_strcasecmp(prev2_wd, "ANALYZE") == 0 ||
			  pg_strcasecmp(prev2_wd, "EXPLAIN") == 0))
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

/* GRANT && REVOKE*/
	/* Complete GRANT/REVOKE with a list of privileges */
	else if (pg_strcasecmp(prev_wd, "GRANT") == 0 ||
			 pg_strcasecmp(prev_wd, "REVOKE") == 0)
	{
		static const char *const list_privileg[] =
		{"SELECT", "INSERT", "UPDATE", "DELETE", "RULE", "REFERENCES",
			"TRIGGER", "CREATE", "CONNECT", "TEMPORARY", "EXECUTE", "USAGE",
		"ALL", NULL};

		COMPLETE_WITH_LIST(list_privileg);
	}
	/* Complete GRANT/REVOKE <sth> with "ON" */
	else if (pg_strcasecmp(prev2_wd, "GRANT") == 0 ||
			 pg_strcasecmp(prev2_wd, "REVOKE") == 0)
		COMPLETE_WITH_CONST("ON");

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
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsv,
								   " UNION SELECT 'DATABASE'"
								   " UNION SELECT 'FUNCTION'"
								   " UNION SELECT 'LANGUAGE'"
								   " UNION SELECT 'SCHEMA'"
								   " UNION SELECT 'TABLESPACE'");

	/* Complete "GRANT/REVOKE * ON * " with "TO" */
	else if ((pg_strcasecmp(prev4_wd, "GRANT") == 0 ||
			  pg_strcasecmp(prev4_wd, "REVOKE") == 0) &&
			 pg_strcasecmp(prev2_wd, "ON") == 0)
	{
		if (pg_strcasecmp(prev_wd, "DATABASE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_databases);
		else if (pg_strcasecmp(prev_wd, "FUNCTION") == 0)
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions, NULL);
		else if (pg_strcasecmp(prev_wd, "LANGUAGE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_languages);
		else if (pg_strcasecmp(prev_wd, "SCHEMA") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
		else if (pg_strcasecmp(prev_wd, "TABLESPACE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
		else if (pg_strcasecmp(prev4_wd, "GRANT") == 0)
			COMPLETE_WITH_CONST("TO");
		else
			COMPLETE_WITH_CONST("FROM");
	}

	/* Complete "GRANT/REVOKE * ON * TO/FROM" with username, GROUP, or PUBLIC */
	else if (pg_strcasecmp(prev3_wd, "ON") == 0 &&
			 ((pg_strcasecmp(prev5_wd, "GRANT") == 0 &&
			   pg_strcasecmp(prev_wd, "TO") == 0) ||
			  (pg_strcasecmp(prev5_wd, "REVOKE") == 0 &&
			   pg_strcasecmp(prev_wd, "FROM") == 0)))
		COMPLETE_WITH_QUERY(Query_for_list_of_grant_roles);

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
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
	/* Complete "INSERT INTO <table> (" with attribute names */
	else if (rl_line_buffer[start - 1] == '(' &&
			 pg_strcasecmp(prev3_wd, "INSERT") == 0 &&
			 pg_strcasecmp(prev2_wd, "INTO") == 0)
		COMPLETE_WITH_ATTR(prev_wd, "");

	/*
	 * Complete INSERT INTO <table> with "VALUES" or "SELECT" or "DEFAULT
	 * VALUES"
	 */
	else if (pg_strcasecmp(prev3_wd, "INSERT") == 0 &&
			 pg_strcasecmp(prev2_wd, "INTO") == 0)
	{
		static const char *const list_INSERT[] =
		{"DEFAULT VALUES", "SELECT", "VALUES", NULL};

		COMPLETE_WITH_LIST(list_INSERT);
	}
	/* Complete INSERT INTO <table> (attribs) with "VALUES" or "SELECT" */
	else if (pg_strcasecmp(prev4_wd, "INSERT") == 0 &&
			 pg_strcasecmp(prev3_wd, "INTO") == 0 &&
			 prev_wd[strlen(prev_wd) - 1] == ')')
	{
		static const char *const list_INSERT[] =
		{"SELECT", "VALUES", NULL};

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
			  pg_strcasecmp(prev_wd, "TABLE")) ||
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
		COMPLETE_WITH_QUERY("SELECT pg_catalog.quote_ident(relname) FROM pg_catalog.pg_listener WHERE substring(pg_catalog.quote_ident(relname),1,%d)='%s'");

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
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
		else if (pg_strcasecmp(prev_wd, "INDEX") == 0)
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes, NULL);
		else if (pg_strcasecmp(prev_wd, "SYSTEM") == 0 ||
				 pg_strcasecmp(prev_wd, "DATABASE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_databases);
	}

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

/* TRUNCATE */
	else if (pg_strcasecmp(prev_wd, "TRUNCATE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);

/* UNLISTEN */
	else if (pg_strcasecmp(prev_wd, "UNLISTEN") == 0)
		COMPLETE_WITH_QUERY("SELECT pg_catalog.quote_ident(relname) FROM pg_catalog.pg_listener WHERE substring(pg_catalog.quote_ident(relname),1,%d)='%s' UNION SELECT '*'");

/* UPDATE */
	/* If prev. word is UPDATE suggest a list of tables */
	else if (pg_strcasecmp(prev_wd, "UPDATE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
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

/*
 * VACUUM [ FULL | FREEZE ] [ VERBOSE ] [ table ]
 * VACUUM [ FULL | FREEZE ] [ VERBOSE ] ANALYZE [ table [ (column [, ...] ) ] ]
 */
	else if (pg_strcasecmp(prev_wd, "VACUUM") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   " UNION SELECT 'FULL'"
								   " UNION SELECT 'FREEZE'"
								   " UNION SELECT 'ANALYZE'"
								   " UNION SELECT 'VERBOSE'");
	else if (pg_strcasecmp(prev2_wd, "VACUUM") == 0 &&
			 (pg_strcasecmp(prev_wd, "FULL") == 0 ||
			  pg_strcasecmp(prev_wd, "FREEZE") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   " UNION SELECT 'ANALYZE'"
								   " UNION SELECT 'VERBOSE'");
	else if (pg_strcasecmp(prev3_wd, "VACUUM") == 0 &&
			 pg_strcasecmp(prev_wd, "ANALYZE") == 0 &&
			 (pg_strcasecmp(prev2_wd, "FULL") == 0 ||
			  pg_strcasecmp(prev2_wd, "FREEZE") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   " UNION SELECT 'VERBOSE'");
	else if (pg_strcasecmp(prev3_wd, "VACUUM") == 0 &&
			 pg_strcasecmp(prev_wd, "VERBOSE") == 0 &&
			 (pg_strcasecmp(prev2_wd, "FULL") == 0 ||
			  pg_strcasecmp(prev2_wd, "FREEZE") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   " UNION SELECT 'ANALYZE'");
	else if (pg_strcasecmp(prev2_wd, "VACUUM") == 0 &&
			 pg_strcasecmp(prev_wd, "VERBOSE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   " UNION SELECT 'ANALYZE'");
	else if (pg_strcasecmp(prev2_wd, "VACUUM") == 0 &&
			 pg_strcasecmp(prev_wd, "ANALYZE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables,
								   " UNION SELECT 'VERBOSE'");
	else if ((pg_strcasecmp(prev_wd, "ANALYZE") == 0 &&
			  pg_strcasecmp(prev2_wd, "VERBOSE") == 0) ||
			 (pg_strcasecmp(prev_wd, "VERBOSE") == 0 &&
			  pg_strcasecmp(prev2_wd, "ANALYZE") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);

/* ANALYZE */
	/* If the previous word is ANALYZE, produce list of tables */
	else if (pg_strcasecmp(prev_wd, "ANALYZE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);

/* WHERE */
	/* Simple case of the word before the where being the table name */
	else if (pg_strcasecmp(prev_wd, "WHERE") == 0)
		COMPLETE_WITH_ATTR(prev2_wd, "");

/* ... FROM ... */
/* TODO: also include SRF ? */
	else if (pg_strcasecmp(prev_wd, "FROM") == 0 &&
			 pg_strcasecmp(prev3_wd, "COPY") != 0 &&
			 pg_strcasecmp(prev3_wd, "\\copy") != 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsv, NULL);


/* Backslash commands */
/* TODO:  \dc \dd \dl */
	else if (strcmp(prev_wd, "\\connect") == 0 || strcmp(prev_wd, "\\c") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_databases);
	else if (strcmp(prev_wd, "\\d") == 0 || strcmp(prev_wd, "\\d+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tisv, NULL);
	else if (strcmp(prev_wd, "\\da") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_aggregates, NULL);
	else if (strcmp(prev_wd, "\\db") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_tablespaces);
	else if (strcmp(prev_wd, "\\dD") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_domains, NULL);
	else if (strcmp(prev_wd, "\\df") == 0 || strcmp(prev_wd, "\\df+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions, NULL);
	else if (strcmp(prev_wd, "\\dF") == 0 || strcmp(prev_wd, "\\dF+") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_configurations);
	else if (strcmp(prev_wd, "\\dFd") == 0 || strcmp(prev_wd, "\\dFd+") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_dictionaries);
	else if (strcmp(prev_wd, "\\dFp") == 0 || strcmp(prev_wd, "\\dFp+") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_parsers);
	else if (strcmp(prev_wd, "\\dFt") == 0 || strcmp(prev_wd, "\\dFt+") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_ts_templates);
	else if (strcmp(prev_wd, "\\di") == 0 || strcmp(prev_wd, "\\di+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes, NULL);
	else if (strcmp(prev_wd, "\\dn") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
	else if (strcmp(prev_wd, "\\dp") == 0 || strcmp(prev_wd, "\\z") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsv, NULL);
	else if (strcmp(prev_wd, "\\ds") == 0 || strcmp(prev_wd, "\\ds+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_sequences, NULL);
	else if (strcmp(prev_wd, "\\dS") == 0 || strcmp(prev_wd, "\\dS+") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_system_relations);
	else if (strcmp(prev_wd, "\\dt") == 0 || strcmp(prev_wd, "\\dt+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables, NULL);
	else if (strcmp(prev_wd, "\\dT") == 0 || strcmp(prev_wd, "\\dT+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes, NULL);
	else if (strcmp(prev_wd, "\\du") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (strcmp(prev_wd, "\\dv") == 0 || strcmp(prev_wd, "\\dv+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views, NULL);
	else if (strcmp(prev_wd, "\\encoding") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_encodings);
	else if (strcmp(prev_wd, "\\h") == 0 || strcmp(prev_wd, "\\help") == 0)
		COMPLETE_WITH_LIST(sql_commands);
	else if (strcmp(prev_wd, "\\password") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_roles);
	else if (strcmp(prev_wd, "\\pset") == 0)
	{
		static const char *const my_list[] =
		{"format", "border", "expanded",
			"null", "fieldsep", "tuples_only", "title", "tableattr", "pager",
		"recordsep", NULL};

		COMPLETE_WITH_LIST(my_list);
	}
	else if (strcmp(prev_wd, "\\cd") == 0 ||
			 strcmp(prev_wd, "\\e") == 0 || strcmp(prev_wd, "\\edit") == 0 ||
			 strcmp(prev_wd, "\\g") == 0 ||
		  strcmp(prev_wd, "\\i") == 0 || strcmp(prev_wd, "\\include") == 0 ||
			 strcmp(prev_wd, "\\o") == 0 || strcmp(prev_wd, "\\out") == 0 ||
			 strcmp(prev_wd, "\\s") == 0 ||
			 strcmp(prev_wd, "\\w") == 0 || strcmp(prev_wd, "\\write") == 0
		)
		matches = completion_matches(text, filename_completion_function);


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
	free(prev_wd);
	free(prev2_wd);
	free(prev3_wd);
	free(prev4_wd);
	free(prev5_wd);

	/* Return our Grand List O' Matches */
	return matches;
}



/* GENERATOR FUNCTIONS

   These functions do all the actual work of completing the input. They get
   passed the text so far and the count how many times they have been called so
   far with the same text.
   If you read the above carefully, you'll see that these don't get called
   directly but through the readline interface.
   The return value is expected to be the full completion of the text, going
   through a list each time, or NULL if there are no more matches. The string
   will be free()'d by readline, so you must run it through strdup() or
   something of that sort.
*/

/* This one gives you one from a list of things you can put after CREATE
   as defined above.
*/
static char *
create_command_generator(const char *text, int state)
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
		if ((pg_strncasecmp(name, text, string_length) == 0) && !words_after_create[list_index - 1].noshow)
			return pg_strdup(name);
	}
	/* if nothing matches, return NULL */
	return NULL;
}

/*
 * This function gives you a list of things you can put after a DROP command.
 * Very similar to create_command_generator, but has an additional entry for
 * OWNED BY.  (We do it this way in order not to duplicate the
 * words_after_create list.)
 */
static char *
drop_command_generator(const char *text, int state)
{
	static int	list_index,
				string_length;
	const char *name;

	if (state == 0)
	{
		/* If this is the first time for this completion, init some values */
		list_index = 0;
		string_length = strlen(text);

		/*
		 * DROP can be followed by "OWNED BY", which is not found in the list
		 * for CREATE matches, so make it the first state. (We do not make it
		 * the last state because it would be more difficult to detect when we
		 * have to return NULL instead.)
		 *
		 * Make sure we advance to the next state.
		 */
		list_index++;
		if (pg_strncasecmp("OWNED", text, string_length) == 0)
			return pg_strdup("OWNED");
	}

	/*
	 * In subsequent attempts, try to complete with the same items we use for
	 * CREATE
	 */
	while ((name = words_after_create[list_index++ - 1].name))
	{
		if ((pg_strncasecmp(name, text, string_length) == 0) && (!words_after_create[list_index - 2].noshow))
			return pg_strdup(name);
	}

	/* if nothing matches, return NULL */
	return NULL;
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


/* This creates a list of matching things, according to a query pointed to
   by completion_charp.
   The query can be one of two kinds:
   - A simple query which must contain a %d and a %s, which will be replaced
   by the string length of the text and the text itself. The query may also
   have another %s in it, which will be replaced by the value of
   completion_info_charp.
	 or:
   - A schema query used for completion of both schema and relation names;
   these are more complex and must contain in the following order:
	 %d %s %d %s %d %s %s %d %s
   where %d is the string length of the text and %s the text itself.

   It is assumed that strings should be escaped to become SQL literals
   (that is, what is in the query is actually ... '%s' ...)

   See top of file for examples of both kinds of query.
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
			 * the input-so-far begins with "pg_".	This is a compromise
			 * between not offering system catalogs for completion at all, and
			 * having them swamp the result when the input is just "p".
			 */
			if (strcmp(completion_squery->catname,
					   "pg_catalog.pg_class c") == 0 &&
				strncmp(text, "pg_", 3) !=0)
			{
				appendPQExpBuffer(&query_buffer,
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
							  string_length, e_text, e_info_charp);
		}

		/* Limit the number of records in the result */
		appendPQExpBuffer(&query_buffer, "\nLIMIT %d",
						  completion_max_records);

		result = exec_query(query_buffer.data);

		termPQExpBuffer(&query_buffer);
		free(e_text);
		if (e_info_charp)
			free(e_info_charp);
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


/* This function returns in order one of a fixed, NULL pointer terminated list
   of strings (if matching). This can be used if there are only a fixed number
   SQL words that can appear at certain spot.
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
	psql_assert(completion_charpp);

	/* Initialization */
	if (state == 0)
	{
		list_index = 0;
		string_length = strlen(text);
		casesensitive = true;
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
			return pg_strdup(item);
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


/* This function returns one fixed string the first time even if it doesn't
   match what's there, and nothing the second time. This should be used if there
   is only one possibility that can appear at a certain spot, so misspellings
   will be overwritten.
   The string to be passed must be in completion_charp.
*/
static char *
complete_from_const(const char *text, int state)
{
	(void) text;				/* We don't care about what was entered
								 * already. */

	psql_assert(completion_charp);
	if (state == 0)
		return pg_strdup(completion_charp);
	else
		return NULL;
}



/* HELPER FUNCTIONS */


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

	if (result != NULL && PQresultStatus(result) != PGRES_TUPLES_OK)
	{
#if 0
		psql_error("tab completion: %s failed - %s\n",
				   query, PQresStatus(PQresultStatus(result)));
#endif
		PQclear(result);
		result = NULL;
	}

	return result;
}



/*
 * Return the word (space delimited) before point. Set skip > 0 to
 * skip that many words; e.g. skip=1 finds the word before the
 * previous one. Return value is NULL or a malloc'ed string.
 */
static char *
previous_word(int point, int skip)
{
	int			i,
				start = 0,
				end = -1,
				inquotes = 0;
	char	   *s;

	while (skip-- >= 0)
	{
		/* first we look for a space before the current word */
		for (i = point; i >= 0; i--)
			if (rl_line_buffer[i] == ' ')
				break;

		/* now find the first non-space which then constitutes the end */
		for (; i >= 0; i--)
			if (rl_line_buffer[i] != ' ')
			{
				end = i;
				break;
			}

		/*
		 * If no end found we return null, because there is no word before the
		 * point
		 */
		if (end == -1)
			return NULL;

		/*
		 * Otherwise we now look for the start. The start is either the last
		 * character before any space going backwards from the end, or it's
		 * simply character 0
		 */
		for (start = end; start > 0; start--)
		{
			if (rl_line_buffer[start] == '"')
				inquotes = !inquotes;
			if ((rl_line_buffer[start - 1] == ' ') && inquotes == 0)
				break;
		}

		point = start;
	}

	/* make a copy */
	s = pg_malloc(end - start + 2);
	strlcpy(s, &rl_line_buffer[start], end - start + 2);

	return s;
}

/* Find the parenthesis after the last word */


static int
find_open_parenthesis(int end)
{
	int			i = end - 1;

	while ((rl_line_buffer[i] != ' ') && (i >= 0))
	{
		if (rl_line_buffer[i] == '(')
			return 1;
		i--;
	}
	while ((rl_line_buffer[i] == ' ') && (i >= 0))
	{
		i--;
	}
	if (rl_line_buffer[i] == '(')
	{
		return 1;
	}
	return 0;

}

#if 0

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
#endif   /* 0 */

#endif   /* USE_READLINE */
