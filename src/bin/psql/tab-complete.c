/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000-2002 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/tab-complete.c,v 1.82 2003/07/29 00:03:18 tgl Exp $
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
 * The number tuples returned gets limited, in most default
 * installations to 101, but if you still don't like this prospect,
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
 * - If you split your queries across lines, this whole things gets
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
#ifdef USE_ASSERT_CHECKING
#include <assert.h>
#endif

#include "libpq-fe.h"

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

#define BUF_SIZE 2048
#define ERROR_QUERY_TOO_LONG	/* empty */


/* Forward declaration of functions */
static char **psql_completion(char *text, int start, int end);
static char *create_command_generator(const char *text, int state);
static char *complete_from_query(const char *text, int state);
static char *complete_from_schema_query(const char *text, int state);
static char *_complete_from_query(int is_schema_query,
								  const char *text, int state);
static char *complete_from_const(const char *text, int state);
static char *complete_from_list(const char *text, int state);

static PGresult *exec_query(char *query);
char	   *quote_file_name(char *text, int match_type, char *quote_pointer);

/*static char * dequote_file_name(char *text, char quote_char);*/
static char *previous_word(int point, int skip);

/* These variables are used to pass information into the completion functions.
   Realizing that this is the cardinal sin of programming, I don't see a better
   way. */
char	   *completion_charp;	/* if you need to pass a string */
char	  **completion_charpp;	/* if you need to pass a list of strings */
char	   *completion_info_charp;		/* if you need to pass another
										 * string */

/* Store how many records from a database query we want to return at most
(implemented via SELECT ... LIMIT xx). */
static int	completion_max_records;


/* Initialize the readline library for our purposes. */
void
initialize_readline(void)
{
	rl_readline_name = pset.progname;
	rl_attempted_completion_function = (void *) psql_completion;

	rl_basic_word_break_characters = "\t\n@$><=;|&{( ";

	completion_max_records = 100;

	/*
	 * There is a variable rl_completion_query_items for this but
	 * apparently it's not defined everywhere.
	 */
}


/*
 * Queries to get lists of names of various kinds of things, possibly
 * restricted to names matching a partially entered name.  In these queries,
 * the %s will be replaced by the text entered so far, the %d by its length.
 */

#define Query_for_list_of_aggregates \
" SELECT DISTINCT proname " \
"   FROM pg_catalog.pg_proc" \
"  WHERE proisagg " \
"    AND substr(proname,1,%d)='%s'" \
"        UNION" \
" SELECT nspname || '.' AS relname" \
"   FROM pg_catalog.pg_namespace" \
"  WHERE substr(nspname,1,%d)='%s'" \
"        UNION" \
" SELECT DISTINCT nspname || '.' || proname AS relname" \
"   FROM pg_catalog.pg_proc p, pg_catalog.pg_namespace n" \
"  WHERE proisagg  " \
"    AND substr(nspname || '.' || proname,1,%d)='%s'" \
"    AND pronamespace = n.oid" \
"    AND ('%s' ~ '^.*\\\\.' "\
"     OR (SELECT TRUE "\
"           FROM pg_catalog.pg_namespace "\
"          WHERE substr(nspname,1,%d)='%s' "\
"         HAVING COUNT(nspname)=1))"

#define Query_for_list_of_attributes \
"SELECT a.attname FROM pg_catalog.pg_attribute a, pg_catalog.pg_class c "\
" WHERE c.oid = a.attrelid "\
"   AND a.attnum > 0 "\
"   AND NOT a.attisdropped "\
"   AND substr(a.attname,1,%d)='%s' "\
"   AND c.relname='%s' "\
"   AND pg_catalog.pg_table_is_visible(c.oid)"

#define Query_for_list_of_databases \
"SELECT datname FROM pg_catalog.pg_database "\
" WHERE substr(datname,1,%d)='%s'"

#define Query_for_list_of_datatypes \
" SELECT pg_catalog.format_type(t.oid, NULL) "\
"   FROM pg_catalog.pg_type t "\
"  WHERE (t.typrelid = 0 "\
"     OR (SELECT c.relkind = 'c' FROM pg_catalog.pg_class c WHERE c.oid = t.typrelid)) "\
"    AND t.typname !~ '^_' "\
"    AND substr(pg_catalog.format_type(t.oid, NULL),1,%d)='%s' "\
"        UNION "\
" SELECT nspname || '.' AS relname "\
"   FROM pg_catalog.pg_namespace "\
"  WHERE substr(nspname,1,%d)='%s' "\
"        UNION "\
" SELECT nspname || '.' || pg_catalog.format_type(t.oid, NULL) AS relname "\
"   FROM pg_catalog.pg_type t, pg_catalog.pg_namespace n "\
"  WHERE(t.typrelid = 0 "\
"     OR (SELECT c.relkind = 'c' FROM pg_catalog.pg_class c WHERE c.oid = t.typrelid)) "\
"    AND t.typname !~ '^_' "\
"    AND substr(nspname || '.' || pg_catalog.format_type(t.oid, NULL),1,%d)='%s' "\
"    AND typnamespace = n.oid "\
"    AND ('%s' ~ '^.*\\\\.' "\
"     OR (SELECT TRUE "\
"           FROM pg_catalog.pg_namespace "\
"          WHERE substr(nspname,1,%d)='%s' "\
"         HAVING COUNT(nspname)=1))"

#define Query_for_list_of_domains \
" SELECT typname "\
"   FROM pg_catalog.pg_type t "\
"  WHERE typtype = 'd' "\
"    AND substr(typname,1,%d)='%s' "\
"        UNION" \
" SELECT nspname || '.' "\
"   FROM pg_catalog.pg_namespace "\
"  WHERE substr(nspname,1,%d)='%s' "\
"        UNION "\
" SELECT nspname || '.' || pg_catalog.format_type(t.oid, NULL) "\
"   FROM pg_catalog.pg_type t, pg_catalog.pg_namespace n "\
"  WHERE typtype = 'd' "\
"    AND substr(nspname || '.' || typname,1,%d)='%s' "\
"    AND typnamespace = n.oid "\
"    AND ('%s' ~ '^.*\\\\.' "\
"     OR (SELECT TRUE "\
"           FROM pg_catalog.pg_namespace "\
"          WHERE substr(nspname,1,%d)='%s' "\
"         HAVING COUNT(nspname)=1))"

#define Query_for_list_of_encodings \
" SELECT DISTINCT pg_catalog.pg_encoding_to_char(conforencoding) "\
"   FROM pg_catalog.pg_conversion "\
"  WHERE substr(pg_catalog.pg_encoding_to_char(conforencoding),1,%d)=UPPER('%s')"

#define Query_for_list_of_functions \
" SELECT DISTINCT proname || '()' "\
"   FROM pg_catalog.pg_proc p, pg_catalog.pg_namespace n "\
"  WHERE substr(proname,1,%d)='%s'"\
"    AND pg_catalog.pg_function_is_visible(p.oid) "\
"    AND pronamespace = n.oid "\
"        UNION "\
" SELECT nspname || '.' "\
"   FROM pg_catalog.pg_namespace "\
"  WHERE substr(nspname,1,%d)='%s' "\
"        UNION "\
" SELECT nspname || '.' || proname "\
"   FROM pg_catalog.pg_proc p, pg_catalog.pg_namespace n "\
"  WHERE substr(nspname || '.' || proname,1,%d)='%s' "\
"    AND pronamespace = n.oid "\
"    AND ('%s' ~ '^.*\\\\.' "\
"     OR (SELECT TRUE "\
"           FROM pg_catalog.pg_namespace "\
"          WHERE substr(nspname,1,%d)='%s' "\
"         HAVING COUNT(nspname)=1))"

#define Query_for_list_of_indexes \
" SELECT relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE relkind='i' "\
"    AND substr(relname,1,%d)='%s' "\
"    AND pg_catalog.pg_table_is_visible(c.oid) "\
"    AND relnamespace = n.oid "\
"    AND n.nspname NOT IN ('pg_catalog', 'pg_toast') "\
"        UNION "\
" SELECT nspname || '.' "\
"   FROM pg_catalog.pg_namespace "\
"  WHERE substr(nspname,1,%d)='%s' "\
"        UNION "\
" SELECT nspname || '.' || relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE relkind='i' "\
"    AND substr(nspname || '.' || relname,1,%d)='%s' "\
"    AND relnamespace = n.oid "\
"    AND ('%s' ~ '^.*\\\\.' "\
"     OR (SELECT TRUE "\
"           FROM pg_catalog.pg_namespace "\
"          WHERE substr(nspname,1,%d)='%s' "\
"         HAVING COUNT(nspname)=1))"


#define Query_for_list_of_languages \
"SELECT lanname "\
"  FROM pg_language "\
" WHERE lanname != 'internal' "\
"   AND substr(lanname,1,%d)='%s' "

#define Query_for_list_of_schemas \
"SELECT nspname FROM pg_catalog.pg_namespace "\
" WHERE substr(nspname,1,%d)='%s'"

#define Query_for_list_of_sequences \
" SELECT relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE relkind='S' "\
"    AND substr(relname,1,%d)='%s' "\
"    AND pg_catalog.pg_table_is_visible(c.oid) "\
"    AND relnamespace = n.oid "\
"    AND n.nspname NOT IN ('pg_catalog', 'pg_toast') "\
"        UNION "\
" SELECT nspname || '.' "\
"   FROM pg_catalog.pg_namespace "\
"  WHERE substr(nspname,1,%d)='%s' "\
"        UNION "\
" SELECT nspname || '.' || relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE relkind='S' "\
"    AND substr(nspname || '.' || relname,1,%d)='%s' "\
"    AND relnamespace = n.oid "\
"    AND ('%s' ~ '^.*\\\\.' "\
"     OR (SELECT TRUE "\
"           FROM pg_catalog.pg_namespace "\
"          WHERE substr(nspname,1,%d)='%s' "\
"         HAVING COUNT(nspname)=1))"

#define Query_for_list_of_system_relations \
"SELECT c.relname FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
" WHERE (c.relkind='r' OR c.relkind='v' OR c.relkind='s' OR c.relkind='S') "\
"   AND substr(c.relname,1,%d)='%s' "\
"   AND pg_catalog.pg_table_is_visible(c.oid)"\
"   AND relnamespace = n.oid "\
"   AND n.nspname = 'pg_catalog'"

#define Query_for_list_of_tables \
" SELECT relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE relkind='r' "\
"    AND substr(relname,1,%d)='%s' "\
"    AND pg_catalog.pg_table_is_visible(c.oid) "\
"    AND relnamespace = n.oid "\
"    AND n.nspname NOT IN ('pg_catalog', 'pg_toast') "\
"        UNION "\
" SELECT nspname || '.' "\
"   FROM pg_catalog.pg_namespace "\
"  WHERE substr(nspname  || '.',1,%d)='%s' "\
"        UNION "\
" SELECT nspname || '.' || relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE relkind='r' "\
"    AND substr(nspname || '.' || relname,1,%d)='%s' "\
"    AND relnamespace = n.oid "\
"    AND ('%s' ~ '^.*\\\\.' "\
"     OR (SELECT TRUE "\
"           FROM pg_catalog.pg_namespace n1 "\
"          WHERE substr(nspname ||'.',1,%d)='%s' "\
"         HAVING COUNT(nspname)=1))"

#define Query_for_list_of_tisv \
" SELECT relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE (relkind='r' OR relkind='i' OR relkind='S' OR relkind='v') "\
"    AND substr(relname,1,%d)='%s' "\
"    AND pg_catalog.pg_table_is_visible(c.oid) "\
"    AND relnamespace = n.oid "\
"    AND n.nspname NOT IN ('pg_catalog', 'pg_toast') "\
"        UNION "\
" SELECT nspname || '.' "\
"   FROM pg_catalog.pg_namespace "\
"  WHERE substr(nspname,1,%d)='%s' "\
"        UNION "\
" SELECT nspname || '.' || relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE (relkind='r' OR relkind='i' OR relkind='S' OR relkind='v') "\
"    AND substr(nspname || '.' || relname,1,%d)='%s' "\
"    AND relnamespace = n.oid "\
"    AND ('%s' ~ '^.*\\\\.' "\
"     OR (SELECT TRUE "\
"           FROM pg_catalog.pg_namespace "\
"          WHERE substr(nspname,1,%d)='%s' "\
"         HAVING COUNT(nspname)=1))"

#define Query_for_list_of_tsv \
" SELECT relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE (relkind='r' OR relkind='S' OR relkind='v') "\
"    AND substr(relname,1,%d)='%s' "\
"    AND pg_catalog.pg_table_is_visible(c.oid) "\
"    AND relnamespace = n.oid "\
"    AND n.nspname NOT IN ('pg_catalog', 'pg_toast') "\
"        UNION "\
" SELECT nspname || '.' "\
"   FROM pg_catalog.pg_namespace "\
"  WHERE substr(nspname,1,%d)='%s' "\
"        UNION "\
" SELECT nspname || '.' || relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE (relkind='r' OR relkind='S' OR relkind='v') "\
"    AND substr(nspname || '.' || relname,1,%d)='%s' "\
"    AND relnamespace = n.oid "\
"    AND ('%s' ~ '^.*\\\\.' "\
"     OR (SELECT TRUE "\
"           FROM pg_catalog.pg_namespace "\
"          WHERE substr(nspname,1,%d)='%s' "\
"         HAVING COUNT(nspname)=1))"

#define Query_for_list_of_views \
" SELECT relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE relkind='v'"\
"    AND substr(relname,1,%d)='%s' "\
"    AND pg_catalog.pg_table_is_visible(c.oid) "\
"    AND relnamespace = n.oid "\
"    AND n.nspname NOT IN ('pg_catalog', 'pg_toast') "\
"        UNION "\
" SELECT nspname || '.' "\
"   FROM pg_catalog.pg_namespace "\
"  WHERE substr(nspname,1,%d)='%s' "\
"        UNION "\
" SELECT nspname || '.' || relname "\
"   FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "\
"  WHERE relkind='v' "\
"    AND substr(nspname || '.' || relname,1,%d)='%s' "\
"    AND relnamespace = n.oid "\
"    AND ('%s' ~ '^.*\\\\.' "\
"     OR (SELECT TRUE "\
"           FROM pg_catalog.pg_namespace "\
"          WHERE substr(nspname,1,%d)='%s' "\
"         HAVING COUNT(nspname)=1))"

#define Query_for_list_of_users \
" SELECT usename "\
"   FROM pg_catalog.pg_user "\
"  WHERE substr(usename,1,%d)='%s'"

/* This is a list of all "things" in Pgsql, which can show up after CREATE or
   DROP; and there is also a query to get a list of them.
*/

#define WITH_SCHEMA 1
#define NO_SCHEMA 0

typedef struct
{
	char	   *name;
	int        with_schema;
	char	   *query;
} pgsql_thing_t;

pgsql_thing_t words_after_create[] = {
	{"AGGREGATE", WITH_SCHEMA, Query_for_list_of_aggregates},
	{"CAST", NO_SCHEMA, NULL},				/* Casts have complex structures for namees, so skip it */
	{"CONVERSION", NO_SCHEMA, "SELECT conname FROM pg_catalog.pg_conversion WHERE substr(conname,1,%d)='%s'"},
	{"DATABASE", NO_SCHEMA, Query_for_list_of_databases},
	{"DOMAIN", WITH_SCHEMA, Query_for_list_of_domains},
	{"FUNCTION", WITH_SCHEMA, Query_for_list_of_functions},
	{"GROUP", NO_SCHEMA, "SELECT groname FROM pg_catalog.pg_group WHERE substr(groname,1,%d)='%s'"},
	{"LANGUAGE", NO_SCHEMA, Query_for_list_of_languages},
	{"INDEX", WITH_SCHEMA,  Query_for_list_of_indexes},
	{"OPERATOR", NO_SCHEMA, NULL},			/* Querying for this is probably not such
								 * a good idea. */
	{"RULE", NO_SCHEMA, "SELECT rulename FROM pg_catalog.pg_rules WHERE substr(rulename,1,%d)='%s'"},
	{"SCHEMA", NO_SCHEMA, Query_for_list_of_schemas},
	{"SEQUENCE", WITH_SCHEMA, Query_for_list_of_sequences},
	{"TABLE", WITH_SCHEMA, Query_for_list_of_tables},
	{"TEMP", NO_SCHEMA, NULL},				/* for CREATE TEMP TABLE ... */
	{"TRIGGER", NO_SCHEMA, "SELECT tgname FROM pg_catalog.pg_trigger WHERE substr(tgname,1,%d)='%s'"},
	{"TYPE", WITH_SCHEMA, Query_for_list_of_datatypes },
	{"UNIQUE", NO_SCHEMA, NULL},			/* for CREATE UNIQUE INDEX ... */
	{"USER", NO_SCHEMA,  Query_for_list_of_users},
	{"VIEW", WITH_SCHEMA, Query_for_list_of_views},
	{NULL, NO_SCHEMA, NULL}				/* end of list */
};


/* A couple of macros to ease typing. You can use these to complete the given
   string with
   1) The results from a query you pass it. (Perhaps one of those above?)
   2) The results from a schema query you pass it.
   3) The items from a null-pointer-terminated list.
   4) A string constant
   5) The list of attributes to the given table.
*/
#define COMPLETE_WITH_QUERY(query) \
do { completion_charp = query; matches = completion_matches(text, complete_from_query); } while(0)
#define COMPLETE_WITH_SCHEMA_QUERY(query) \
do { completion_charp = query; matches = completion_matches(text, complete_from_schema_query); } while(0)
#define COMPLETE_WITH_LIST(list) \
do { completion_charpp = list; matches = completion_matches(text, complete_from_list); } while(0)
#define COMPLETE_WITH_CONST(string) \
do { completion_charp = string; matches = completion_matches(text, complete_from_const); } while(0)
#define COMPLETE_WITH_ATTR(table) \
do {completion_charp = Query_for_list_of_attributes; completion_info_charp = table; matches = completion_matches(text, complete_from_query); } while(0)


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
			   *prev4_wd;

	static char *sql_commands[] = {
		"ABORT", "ALTER", "ANALYZE", "BEGIN", "CHECKPOINT", "CLOSE", "CLUSTER", "COMMENT",
		"COMMIT", "COPY", "CREATE", "DEALLOCATE", "DECLARE", "DELETE", "DROP", "EXECUTE",
		"EXPLAIN", "FETCH", "GRANT", "INSERT", "LISTEN", "LOAD", "LOCK", "MOVE", "NOTIFY",
		"PREPARE", "REINDEX", "RESET", "REVOKE", "ROLLBACK", "SELECT", "SET", "SHOW",
		"TRUNCATE", "UNLISTEN", "UPDATE", "VACUUM", NULL
	};

	static char *pgsql_variables[] = {
		/* these SET arguments are known in gram.y */
		"CONSTRAINTS",
		"NAMES",
		"SESSION",
		"TRANSACTION",

		/*
		 * the rest should match USERSET and possibly SUSET entries in
		 * backend/utils/misc/guc.c.
		 */
		"add_missing_from",
		"australian_timezones",
		"client_encoding",
		"client_min_messages",
		"commit_delay",
		"commit_siblings",
		"cpu_index_tuple_cost",
		"cpu_operator_cost",
		"cpu_tuple_cost",
		"DateStyle",
		"deadlock_timeout",
		"debug_pretty_print",
		"debug_print_parse",
		"debug_print_plan",
		"debug_print_rewritten",
		"default_statistics_target",
		"default_transaction_isolation",
		"default_transaction_read_only",
		"dynamic_library_path",
		"effective_cache_size",
		"enable_hashagg",
		"enable_hashjoin",
		"enable_indexscan",
		"enable_mergejoin",
		"enable_nestloop",
		"enable_seqscan",
		"enable_sort",
		"enable_tidscan",
		"explain_pretty_print",
		"extra_float_digits",
		"from_collapse_limit",
		"fsync",
		"geqo",
		"geqo_effort",
		"geqo_generations",
		"geqo_pool_size",
		"geqo_random_seed",
		"geqo_selection_bias",
		"geqo_threshold",
		"join_collapse_limit",
		"krb_server_keyfile",
		"lc_messages",
		"lc_monetary",
		"lc_numeric",
		"lc_time",
		"log_duration",
		"log_error_verbosity",
		"log_executor_stats",
		"log_min_duration_statement",
		"log_min_error_statement",
		"log_min_messages",
		"log_parser_stats",
		"log_planner_stats",
		"log_statement",
		"log_statement_stats",
		"max_connections",
		"max_expr_depth",
		"max_files_per_process",
		"max_fsm_pages",
		"max_fsm_relations",
		"max_locks_per_transaction",
		"password_encryption",
		"port",
		"random_page_cost",
		"regex_flavor",
		"search_path",
		"shared_buffers",
		"seed",
		"server_encoding",
		"sort_mem",
		"sql_inheritance",
		"ssl",
		"statement_timeout",
		"stats_block_level",
		"stats_command_string",
		"stats_reset_on_server_start",
		"stats_row_level",
		"stats_start_collector",
		"superuser_reserved_connections",
		"syslog",
		"syslog_facility",
		"syslog_ident",
		"tcpip_socket",
		"TimeZone",
		"trace_notify",
		"transform_null_equals",
		"unix_socket_directory",
		"unix_socket_group",
		"unix_socket_permissions",
		"vacuum_mem",
		"wal_buffers",
		"wal_debug",
		"wal_sync_method",
		NULL
	};

	static char *backslash_commands[] = {
		"\\a", "\\connect", "\\C", "\\cd", "\\copy", "\\copyright", 
		"\\d",  "\\da", "\\dc", "\\dC", "\\dd", "\\dD", "\\df", "\\di",
		"\\dl", "\\dn", "\\do", "\\dp", "\\ds", "\\dS", "\\dt", "\\dT", 
		"\\dv", "\\du",
		"\\e", "\\echo", "\\encoding",
		"\\f", "\\g", "\\h", "\\help", "\\H", "\\i", "\\l",
		"\\lo_import", "\\lo_export", "\\lo_list", "\\lo_unlink",
		"\\o", "\\p", "\\pset", "\\q", "\\qecho", "\\r", "\\set", "\\t", "\\T",
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
	 * words. According to those we'll make some smart decisions on what
	 * the user is probably intending to type. TODO: Use strtokx() to do
	 * this.
	 */
	prev_wd = previous_word(start, 0);
	prev2_wd = previous_word(start, 1);
	prev3_wd = previous_word(start, 2);
	prev4_wd = previous_word(start, 3);

	/* If a backslash command was started, continue */
	if (text[0] == '\\')
		COMPLETE_WITH_LIST(backslash_commands);

	/* If no previous word, suggest one of the basic sql commands */
	else if (!prev_wd)
		COMPLETE_WITH_LIST(sql_commands);

/* CREATE or DROP but not ALTER TABLE sth DROP */
	/* complete with something you can create or drop */
	else if (strcasecmp(prev_wd, "CREATE") == 0 || 
			 (strcasecmp(prev_wd, "DROP") == 0 &&
			  strcasecmp(prev3_wd,"TABLE") != 0 ))
        matches = completion_matches(text, create_command_generator);

/* ALTER */
    /* complete with what you can alter (TABLE, GROUP, USER, ...) 
     * unless we're in ALTER TABLE sth ALTER*/
    else if (strcasecmp(prev_wd, "ALTER") == 0  &&
			 strcasecmp(prev3_wd, "TABLE") != 0 )
	{
		char	   *list_ALTER[] = {"DATABASE", "GROUP", "SCHEMA", "TABLE",
									"TRIGGER", "USER", NULL};

		COMPLETE_WITH_LIST(list_ALTER);
	}

	/* ALTER DATABASE <name> */
	else if (strcasecmp(prev3_wd, "ALTER") == 0 &&
			 strcasecmp(prev2_wd, "DATABASE") == 0)
	{
		char	   *list_ALTERDATABASE[] = {"RESET", "SET", NULL};

		COMPLETE_WITH_LIST(list_ALTERDATABASE);
	}
	/* ALTER TRIGGER <name>, add ON */
	else if (strcasecmp(prev3_wd, "ALTER") == 0 &&
			 strcasecmp(prev2_wd, "TRIGGER") == 0)
		COMPLETE_WITH_CONST("ON");

	/*
	 * If we have ALTER TRIGGER <sth> ON, then add the correct tablename
	 */
	else if (strcasecmp(prev4_wd, "ALTER") == 0 &&
			 strcasecmp(prev3_wd, "TRIGGER") == 0 &&
			 strcasecmp(prev_wd, "ON") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

	/*
	 * If we detect ALTER TABLE <name>, suggest either ADD, DROP, ALTER,
	 * RENAME, or OWNER
	 */
	else if (strcasecmp(prev3_wd, "ALTER") == 0 &&
			 strcasecmp(prev2_wd, "TABLE") == 0)
	{
		char	   *list_ALTER2[] = {"ADD", "ALTER", "DROP", "RENAME",
									 "OWNER TO", NULL};

		COMPLETE_WITH_LIST(list_ALTER2);
	}
	/* If we have TABLE <sth> ALTER|RENAME, provide list of columns */
	else if (strcasecmp(prev3_wd, "TABLE") == 0 &&
			 (strcasecmp(prev_wd, "ALTER") == 0 ||
			  strcasecmp(prev_wd, "RENAME") == 0))
		COMPLETE_WITH_ATTR(prev2_wd);

	/* If we have TABLE <sth> DROP, provide COLUMN or CONSTRAINT */
	else if (strcasecmp(prev3_wd, "TABLE") == 0 &&
			 strcasecmp(prev_wd, "DROP")  == 0)
	{
		char	   *list_TABLEDROP[] = {"COLUMN", "CONSTRAINT", NULL};
		COMPLETE_WITH_LIST(list_TABLEDROP);
	}
	/* If we have TABLE <sth> DROP COLUMN, provide list of columns */
	else if (strcasecmp(prev4_wd, "TABLE") == 0 &&
			 strcasecmp(prev2_wd, "DROP") == 0 && 
			 strcasecmp(prev_wd, "COLUMN") == 0)
		COMPLETE_WITH_ATTR(prev3_wd);

	/* complete ALTER GROUP <foo> with ADD or DROP */
	else if (strcasecmp(prev3_wd, "ALTER") == 0 &&
			 strcasecmp(prev2_wd, "GROUP") == 0)
	{
		char	   *list_ALTERGROUP[] = {"ADD", "DROP", NULL};

		COMPLETE_WITH_LIST(list_ALTERGROUP);
	}
	/* complete ALTER GROUP <foo> ADD|DROP with USER */
	else if (strcasecmp(prev4_wd, "ALTER") == 0 &&
			 strcasecmp(prev3_wd, "GROUP") == 0 &&
			 (strcasecmp(prev_wd, "ADD") == 0 ||
			  strcasecmp(prev_wd, "DROP") == 0))
		COMPLETE_WITH_CONST("USER");
	/* complete {ALTER} GROUP <foo> ADD|DROP USER with a user name */
	else if (strcasecmp(prev4_wd, "GROUP") == 0 &&
			 (strcasecmp(prev2_wd, "ADD") == 0 ||
			  strcasecmp(prev2_wd, "DROP") == 0) &&
			 strcasecmp(prev_wd, "USER") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_users);

/* ANALYZE */
	/* If the previous word is ANALYZE, produce list of tables. */
	else if (strcasecmp(prev_wd, "ANALYZE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	/* If we have ANALYZE <table>, complete with semicolon. */
	else if (strcasecmp(prev2_wd, "ANALYZE") == 0)
		COMPLETE_WITH_CONST(";");

/* CLUSTER */
	/* If the previous word is CLUSTER, produce list of indexes. */
	else if (strcasecmp(prev_wd, "CLUSTER") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes);
	/* If we have CLUSTER <sth>, then add "ON" */
	else if (strcasecmp(prev2_wd, "CLUSTER") == 0)
		COMPLETE_WITH_CONST("ON");

	/*
	 * If we have CLUSTER <sth> ON, then add the correct tablename as
	 * well.
	 */
	else if (strcasecmp(prev3_wd, "CLUSTER") == 0 &&
			 strcasecmp(prev_wd, "ON") == 0)
	{
		char		query_buffer[BUF_SIZE];		/* Some room to build
												 * queries. */

		if (snprintf(query_buffer, BUF_SIZE,
					 "SELECT c1.relname FROM pg_catalog.pg_class c1, pg_catalog.pg_class c2, pg_catalog.pg_index i WHERE c1.oid=i.indrelid and i.indexrelid=c2.oid and c2.relname='%s' and pg_catalog.pg_table_is_visible(c2.oid)",
					 prev2_wd) == -1)
			ERROR_QUERY_TOO_LONG;
		else
			COMPLETE_WITH_QUERY(query_buffer);
	}

/* COMMENT */
	else if (strcasecmp(prev_wd, "COMMENT") == 0)
		COMPLETE_WITH_CONST("ON");
	else if (strcasecmp(prev2_wd, "COMMENT") == 0 &&
			 strcasecmp(prev_wd, "ON") == 0)
	{
		char	   *list_COMMENT[] =
		{"DATABASE", "INDEX", "RULE", "SCHEMA", "SEQUENCE", "TABLE",
		 "TYPE", "VIEW", "COLUMN", "AGGREGATE", "FUNCTION", "OPERATOR",
		 "TRIGGER", "CONSTRAINT", "DOMAIN", NULL};

		COMPLETE_WITH_LIST(list_COMMENT);
	}
	else if (strcasecmp(prev4_wd, "COMMENT") == 0 &&
			 strcasecmp(prev3_wd, "ON") == 0)
		COMPLETE_WITH_CONST("IS");

/* COPY */

	/*
	 * If we have COPY [BINARY] (which you'd have to type yourself), offer
	 * list of tables (Also cover the analogous backslash command)
	 */
	else if (strcasecmp(prev_wd, "COPY") == 0 ||
			 strcasecmp(prev_wd, "\\copy") == 0 ||
			 (strcasecmp(prev2_wd, "COPY") == 0 &&
			  strcasecmp(prev_wd, "BINARY") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	/* If we have COPY|BINARY <sth>, complete it with "TO" or "FROM" */
	else if (strcasecmp(prev2_wd, "COPY") == 0 ||
			 strcasecmp(prev2_wd, "\\copy") == 0 ||
			 strcasecmp(prev2_wd, "BINARY") == 0)
	{
		char	   *list_FROMTO[] = {"FROM", "TO", NULL};

		COMPLETE_WITH_LIST(list_FROMTO);
	}

/* CREATE INDEX */
	/* First off we complete CREATE UNIQUE with "INDEX" */
	else if (strcasecmp(prev2_wd, "CREATE") == 0 &&
			 strcasecmp(prev_wd, "UNIQUE") == 0)
		COMPLETE_WITH_CONST("INDEX");
	/* If we have CREATE|UNIQUE INDEX <sth>, then add "ON" */
	else if (strcasecmp(prev2_wd, "INDEX") == 0 &&
			 (strcasecmp(prev3_wd, "CREATE") == 0 ||
			  strcasecmp(prev3_wd, "UNIQUE") == 0))
		COMPLETE_WITH_CONST("ON");
	/* Complete ... INDEX <name> ON with a list of tables  */
	else if (strcasecmp(prev3_wd, "INDEX") == 0 &&
			 strcasecmp(prev_wd, "ON") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

	/*
	 * Complete INDEX <name> ON <table> with a list of table columns
	 * (which should really be in parens)
	 */
	else if (strcasecmp(prev4_wd, "INDEX") == 0 &&
			 strcasecmp(prev2_wd, "ON") == 0)
		COMPLETE_WITH_ATTR(prev_wd);
	/* same if you put in USING */
	else if (strcasecmp(prev4_wd, "ON") == 0 &&
			 strcasecmp(prev2_wd, "USING") == 0)
		COMPLETE_WITH_ATTR(prev3_wd);
	/* Complete USING with an index method */
	else if (strcasecmp(prev_wd, "USING") == 0)
	{
		char	   *index_mth[] = {"BTREE", "RTREE", "HASH", "GIST", NULL};

		COMPLETE_WITH_LIST(index_mth);
	}

/* CREATE RULE */
	/* Complete "CREATE RULE <sth>" with "AS" */
	else if (strcasecmp(prev3_wd, "CREATE") == 0 &&
			 strcasecmp(prev2_wd, "RULE") == 0)
		COMPLETE_WITH_CONST("AS");
	/* Complete "CREATE RULE <sth> AS with "ON" */
	else if (strcasecmp(prev4_wd, "CREATE") == 0 &&
			 strcasecmp(prev3_wd, "RULE") == 0 &&
			 strcasecmp(prev_wd, "AS") == 0)
		COMPLETE_WITH_CONST("ON");
	/* Complete "RULE * AS ON" with SELECT|UPDATE|DELETE|INSERT */
	else if (strcasecmp(prev4_wd, "RULE") == 0 &&
			 strcasecmp(prev2_wd, "AS") == 0 &&
			 strcasecmp(prev_wd, "ON") == 0)
	{
		char	   *rule_events[] = {"SELECT", "UPDATE", "INSERT",
									 "DELETE", NULL};

		COMPLETE_WITH_LIST(rule_events);
	}
	/* Complete "AS ON <sth with a 'T' :)>" with a "TO" */
	else if (strcasecmp(prev3_wd, "AS") == 0 &&
			 strcasecmp(prev2_wd, "ON") == 0 &&
			 (toupper((unsigned char) prev_wd[4]) == 'T' ||
			  toupper((unsigned char) prev_wd[5]) == 'T'))
		COMPLETE_WITH_CONST("TO");
	/* Complete "AS ON <sth> TO" with a table name */
	else if (strcasecmp(prev4_wd, "AS") == 0 &&
			 strcasecmp(prev3_wd, "ON") == 0 &&
			 strcasecmp(prev_wd, "TO") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

/* CREATE TABLE */
	/* Complete CREATE TEMP with "TABLE" */
	else if (strcasecmp(prev2_wd, "CREATE") == 0 &&
			 strcasecmp(prev_wd, "TEMP") == 0)
		COMPLETE_WITH_CONST("TABLE");

/* CREATE TRIGGER */
	/* is on the agenda . . . */

/* CREATE VIEW */
	/* Complete "CREATE VIEW <name>" with "AS" */
	else if (strcasecmp(prev3_wd, "CREATE") == 0 &&
			 strcasecmp(prev2_wd, "VIEW") == 0)
		COMPLETE_WITH_CONST("AS");
	/* Complete "CREATE VIEW <sth> AS with "SELECT" */
	else if (strcasecmp(prev4_wd, "CREATE") == 0 &&
			 strcasecmp(prev3_wd, "VIEW") == 0 &&
			 strcasecmp(prev_wd, "AS") == 0)
		COMPLETE_WITH_CONST("SELECT");

/* DELETE */

	/*
	 * Complete DELETE with FROM (only if the word before that is not "ON"
	 * (cf. rules) or "BEFORE" or "AFTER" (cf. triggers) or GRANT)
	 */
	else if (strcasecmp(prev_wd, "DELETE") == 0 &&
			 !(strcasecmp(prev2_wd, "ON") == 0 ||
			   strcasecmp(prev2_wd, "GRANT") == 0 ||
			   strcasecmp(prev2_wd, "BEFORE") == 0 ||
			   strcasecmp(prev2_wd, "AFTER") == 0))
		COMPLETE_WITH_CONST("FROM");
	/* Complete DELETE FROM with a list of tables */
	else if (strcasecmp(prev2_wd, "DELETE") == 0 &&
			 strcasecmp(prev_wd, "FROM") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	/* Complete DELETE FROM <table> with "WHERE" (perhaps a safe idea?) */
	else if (strcasecmp(prev3_wd, "DELETE") == 0 &&
			 strcasecmp(prev2_wd, "FROM") == 0)
		COMPLETE_WITH_CONST("WHERE");

/* EXPLAIN */

	/*
	 * Complete EXPLAIN [VERBOSE] (which you'd have to type yourself) with
	 * the list of SQL commands
	 */
	else if (strcasecmp(prev_wd, "EXPLAIN") == 0 ||
			 (strcasecmp(prev2_wd, "EXPLAIN") == 0 &&
			  strcasecmp(prev_wd, "VERBOSE") == 0))
		COMPLETE_WITH_LIST(sql_commands);

/* FETCH && MOVE */
	/* Complete FETCH with one of FORWARD, BACKWARD, RELATIVE */
	else if (strcasecmp(prev_wd, "FETCH") == 0 ||
			 strcasecmp(prev_wd, "MOVE") == 0)
	{
		char	   *list_FETCH1[] = {"FORWARD", "BACKWARD", "RELATIVE", NULL};

		COMPLETE_WITH_LIST(list_FETCH1);
	}
	/* Complete FETCH <sth> with one of ALL, NEXT, PRIOR */
	else if (strcasecmp(prev2_wd, "FETCH") == 0 ||
			 strcasecmp(prev2_wd, "MOVE") == 0)
	{
		char	   *list_FETCH2[] = {"ALL", "NEXT", "PRIOR", NULL};

		COMPLETE_WITH_LIST(list_FETCH2);
	}

	/*
	 * Complete FETCH <sth1> <sth2> with "FROM" or "TO". (Is there a
	 * difference? If not, remove one.)
	 */
	else if (strcasecmp(prev3_wd, "FETCH") == 0 ||
			 strcasecmp(prev3_wd, "MOVE") == 0)
	{
		char	   *list_FROMTO[] = {"FROM", "TO", NULL};

		COMPLETE_WITH_LIST(list_FROMTO);
	}

/* GRANT && REVOKE*/
	/* Complete GRANT/REVOKE with a list of privileges */
	else if (strcasecmp(prev_wd, "GRANT") == 0 ||
			 strcasecmp(prev_wd, "REVOKE") == 0)
	{
		char	   *list_privileg[] = {"SELECT", "INSERT", "UPDATE", "DELETE", "RULE", "REFERENCES", "TRIGGER", "CREATE", "TEMPORARY", "EXECUTE", "USAGE", "ALL", NULL};

		COMPLETE_WITH_LIST(list_privileg);
	}
	/* Complete GRANT/REVOKE <sth> with "ON" */
	else if (strcasecmp(prev2_wd, "GRANT") == 0 ||
			 strcasecmp(prev2_wd, "REVOKE") == 0)
		COMPLETE_WITH_CONST("ON");

	/*
	 * Complete GRANT/REVOKE <sth> ON with a list of tables, views,
	 * sequences, and indexes
	 *
	 * keywords DATABASE, FUNCTION, LANGUAGE, SCHEMA added to query result
     * via UNION; seems to work intuitively
     *
     * Note: GRANT/REVOKE can get quite complex; tab-completion as implemented
     * here will only work if the privilege list contains exactly one privilege
	 */
	else if ((strcasecmp(prev3_wd, "GRANT") == 0 ||
			  strcasecmp(prev3_wd, "REVOKE") == 0) &&
			 strcasecmp(prev_wd, "ON") == 0)
		COMPLETE_WITH_QUERY("SELECT relname FROM pg_catalog.pg_class c, pg_catalog.pg_namespace n "
							" WHERE relkind in ('r','S','v')  "
							"   AND substr(relname,1,%d)='%s' "
                            "   AND pg_catalog.pg_table_is_visible(c.oid) "
							"   AND relnamespace = n.oid "
							"   AND n.nspname NOT IN ('pg_catalog', 'pg_toast') "
                            " UNION "
                            "SELECT 'DATABASE' AS relname "
                            " UNION "
                            "SELECT 'FUNCTION' AS relname "
                            " UNION "
                            "SELECT 'LANGUAGE' AS relname "
                            " UNION "
                            "SELECT 'SCHEMA' AS relname ");

	/* Complete "GRANT/REVOKE * ON * " with "TO" */
	else if ((strcasecmp(prev4_wd, "GRANT") == 0 || 
			  strcasecmp(prev4_wd, "REVOKE") == 0) &&
			 strcasecmp(prev2_wd, "ON") == 0)
	{
		if(strcasecmp(prev_wd, "DATABASE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_databases);
		else if(strcasecmp(prev_wd, "FUNCTION") == 0)
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions);
		else if(strcasecmp(prev_wd, "LANGUAGE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_languages);
		else if(strcasecmp(prev_wd, "SCHEMA") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
		else
			COMPLETE_WITH_CONST("TO");
	}

	/*
	 * TODO: to complete with user name we need prev5_wd -- wait for a
	 * more general solution there
     * same for GRANT <sth> ON { DATABASE | FUNCTION | LANGUAGE | SCHEMA } xxx TO
	 */

/* INSERT */
	/* Complete INSERT with "INTO" */
	else if (strcasecmp(prev_wd, "INSERT") == 0)
		COMPLETE_WITH_CONST("INTO");
	/* Complete INSERT INTO with table names */
	else if (strcasecmp(prev2_wd, "INSERT") == 0 &&
			 strcasecmp(prev_wd, "INTO") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	/* Complete "INSERT INTO <table> (" with attribute names */
	else if (rl_line_buffer[start - 1] == '(' &&
			 strcasecmp(prev3_wd, "INSERT") == 0 &&
			 strcasecmp(prev2_wd, "INTO") == 0)
		COMPLETE_WITH_ATTR(prev_wd);

	/*
	 * Complete INSERT INTO <table> with "VALUES" or "SELECT" or "DEFAULT
	 * VALUES"
	 */
	else if (strcasecmp(prev3_wd, "INSERT") == 0 &&
			 strcasecmp(prev2_wd, "INTO") == 0)
	{
		char	   *list_INSERT[] = {"DEFAULT VALUES", "SELECT", "VALUES", NULL};

		COMPLETE_WITH_LIST(list_INSERT);
	}
	/* Complete INSERT INTO <table> (attribs) with "VALUES" or "SELECT" */
	else if (strcasecmp(prev4_wd, "INSERT") == 0 &&
			 strcasecmp(prev3_wd, "INTO") == 0 &&
			 prev_wd[strlen(prev_wd) - 1] == ')')
	{
		char	   *list_INSERT[] = {"SELECT", "VALUES", NULL};

		COMPLETE_WITH_LIST(list_INSERT);
	}

	/* Insert an open parenthesis after "VALUES" */
	else if (strcasecmp(prev_wd, "VALUES") == 0 &&
			 strcasecmp(prev2_wd, "DEFAULT") != 0)
		COMPLETE_WITH_CONST("(");

/* LOCK */
	/* Complete LOCK [TABLE] with a list of tables */
	else if (strcasecmp(prev_wd, "LOCK") == 0 ||
	 		 (strcasecmp(prev_wd, "TABLE") == 0 &&
			  strcasecmp(prev2_wd, "LOCK") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

	/* For the following, handle the case of a single table only for now */

	/* Complete LOCK [TABLE] <table> with "IN" */
	else if ((strcasecmp(prev2_wd, "LOCK") == 0 &&
			  strcasecmp(prev_wd, "TABLE")) ||
			 (strcasecmp(prev2_wd, "TABLE") == 0 &&
			  strcasecmp(prev3_wd, "LOCK") == 0))
		COMPLETE_WITH_CONST("IN");

	/* Complete LOCK [TABLE] <table> IN with a lock mode */
	else if (strcasecmp(prev_wd, "IN") == 0 &&
			 (strcasecmp(prev3_wd, "LOCK") == 0 ||
			  (strcasecmp(prev3_wd, "TABLE") == 0 &&
			   strcasecmp(prev4_wd, "LOCK") == 0)))
	{
		char	   *lock_modes[] = {"ACCESS SHARE MODE",
			"ROW SHARE MODE", "ROW EXCLUSIVE MODE",
			"SHARE UPDATE EXCLUSIVE MODE", "SHARE MODE",
			"SHARE ROW EXCLUSIVE MODE",
			"EXCLUSIVE MODE", "ACCESS EXCLUSIVE MODE", NULL};

		COMPLETE_WITH_LIST(lock_modes);
	}

/* NOTIFY */
	else if (strcasecmp(prev_wd, "NOTIFY") == 0)
		COMPLETE_WITH_QUERY("SELECT relname FROM pg_catalog.pg_listener WHERE substr(relname,1,%d)='%s'");

/* REINDEX */
	else if (strcasecmp(prev_wd, "REINDEX") == 0)
	{
		char	   *list_REINDEX[] = {"TABLE", "DATABASE", "INDEX", NULL};

		COMPLETE_WITH_LIST(list_REINDEX);
	}
	else if (strcasecmp(prev2_wd, "REINDEX") == 0)
	{
		if (strcasecmp(prev_wd, "TABLE") == 0)
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
		else if (strcasecmp(prev_wd, "DATABASE") == 0)
			COMPLETE_WITH_QUERY(Query_for_list_of_databases);
		else if (strcasecmp(prev_wd, "INDEX") == 0)
			COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes);
	}

/* SELECT */
	/* naah . . . */

/* SET, RESET, SHOW */
	/* Complete with a variable name */
	else if ((strcasecmp(prev_wd, "SET") == 0 &&
			  strcasecmp(prev3_wd, "UPDATE") != 0) ||
			 strcasecmp(prev_wd, "RESET") == 0 ||
			 strcasecmp(prev_wd, "SHOW") == 0)
		COMPLETE_WITH_LIST(pgsql_variables);
	/* Complete "SET TRANSACTION" */
	else if ((strcasecmp(prev2_wd, "SET") == 0 &&
			  strcasecmp(prev_wd, "TRANSACTION") == 0) ||
			 (strcasecmp(prev4_wd, "SESSION") == 0 &&
			  strcasecmp(prev3_wd, "CHARACTERISTICS") == 0 &&
			  strcasecmp(prev2_wd, "AS") == 0 &&
			  strcasecmp(prev_wd, "TRANSACTION") == 0))
	{
		char	   *my_list[] = {"ISOLATION", "READ", NULL};

		COMPLETE_WITH_LIST(my_list);
	}
	else if (strcasecmp(prev3_wd, "SET") == 0 &&
			 strcasecmp(prev2_wd, "TRANSACTION") == 0 &&
			 strcasecmp(prev_wd, "ISOLATION") == 0)
		COMPLETE_WITH_CONST("LEVEL");
	else if ((strcasecmp(prev4_wd, "SET") == 0 ||
			  strcasecmp(prev4_wd, "AS") == 0) &&
			 strcasecmp(prev3_wd, "TRANSACTION") == 0 &&
			 strcasecmp(prev2_wd, "ISOLATION") == 0 &&
			 strcasecmp(prev_wd, "LEVEL") == 0)
	{
		char	   *my_list[] = {"READ", "SERIALIZABLE", NULL};

		COMPLETE_WITH_LIST(my_list);
	}
	else if (strcasecmp(prev4_wd, "TRANSACTION") == 0 &&
			 strcasecmp(prev3_wd, "ISOLATION") == 0 &&
			 strcasecmp(prev2_wd, "LEVEL") == 0 &&
			 strcasecmp(prev_wd, "READ") == 0)
		COMPLETE_WITH_CONST("COMMITTED");
	else if ((strcasecmp(prev3_wd, "SET") == 0 ||
			  strcasecmp(prev3_wd, "AS") == 0) &&
			 strcasecmp(prev2_wd, "TRANSACTION") == 0 &&
			 strcasecmp(prev_wd, "READ") == 0)
	{
		char	   *my_list[] = {"ONLY", "WRITE", NULL};

		COMPLETE_WITH_LIST(my_list);
	}
	/* Complete SET CONSTRAINTS <foo> with DEFERRED|IMMEDIATE */
	else if (strcasecmp(prev3_wd, "SET") == 0 &&
			 strcasecmp(prev2_wd, "CONSTRAINTS") == 0)
	{
		char	   *constraint_list[] = {"DEFERRED", "IMMEDIATE", NULL};

		COMPLETE_WITH_LIST(constraint_list);
	}
	/* Complete SET SESSION with AUTHORIZATION or CHARACTERISTICS... */
	else if (strcasecmp(prev2_wd, "SET") == 0 &&
			 strcasecmp(prev_wd, "SESSION") == 0)
	{
		char	   *my_list[] = {"AUTHORIZATION",
			"CHARACTERISTICS AS TRANSACTION",
		NULL};

		COMPLETE_WITH_LIST(my_list);
	}
	/* Complete SET SESSION AUTHORIZATION with username */
	else if (strcasecmp(prev3_wd, "SET") == 0
			 && strcasecmp(prev2_wd, "SESSION") == 0
			 && strcasecmp(prev_wd, "AUTHORIZATION") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_users);
	/* Complete SET <var> with "TO" */
	else if (strcasecmp(prev2_wd, "SET") == 0 &&
			 strcasecmp(prev4_wd, "UPDATE") != 0)
		COMPLETE_WITH_CONST("TO");
	/* Suggest possible variable values */
	else if (strcasecmp(prev3_wd, "SET") == 0 &&
		   (strcasecmp(prev_wd, "TO") == 0 || strcmp(prev_wd, "=") == 0))
	{
		if (strcasecmp(prev2_wd, "DateStyle") == 0)
		{
			char	   *my_list[] = {"ISO", "SQL", "Postgres", "German",
									 "YMD", "DMY", "MDY",
									 "US", "European", "NonEuropean",
									 "DEFAULT", NULL};

			COMPLETE_WITH_LIST(my_list);
		}
		else if (strcasecmp(prev2_wd, "GEQO") == 0)
		{
			char	   *my_list[] = {"ON", "OFF", "DEFAULT", NULL};

			COMPLETE_WITH_LIST(my_list);
		}
		else
		{
			char	   *my_list[] = {"DEFAULT", NULL};

			COMPLETE_WITH_LIST(my_list);
		}
	}

/* TRUNCATE */
	else if (strcasecmp(prev_wd, "TRUNCATE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

/* UNLISTEN */
	else if (strcasecmp(prev_wd, "UNLISTEN") == 0)
		COMPLETE_WITH_QUERY("SELECT relname FROM pg_catalog.pg_listener WHERE substr(relname,1,%d)='%s' UNION SELECT '*'::name");

/* UPDATE */
	/* If prev. word is UPDATE suggest a list of tables */
	else if (strcasecmp(prev_wd, "UPDATE") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	/* Complete UPDATE <table> with "SET" */
	else if (strcasecmp(prev2_wd, "UPDATE") == 0)
		COMPLETE_WITH_CONST("SET");

	/*
	 * If the previous word is SET (and it wasn't caught above as the
	 * _first_ word) the word before it was (hopefully) a table name and
	 * we'll now make a list of attributes.
	 */
	else if (strcasecmp(prev_wd, "SET") == 0)
		COMPLETE_WITH_ATTR(prev2_wd);

/* VACUUM */
	else if (strcasecmp(prev_wd, "VACUUM") == 0)
		COMPLETE_WITH_QUERY("SELECT relname FROM pg_catalog.pg_class WHERE relkind='r' and substr(relname,1,%d)='%s' and pg_catalog.pg_table_is_visible(oid) UNION SELECT 'FULL'::name UNION SELECT 'ANALYZE'::name");
	else if (strcasecmp(prev2_wd, "VACUUM") == 0 &&
			 (strcasecmp(prev_wd, "FULL") == 0 ||
			  strcasecmp(prev_wd, "ANALYZE") == 0))
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);

/* WHERE */
	/* Simple case of the word before the where being the table name */
	else if (strcasecmp(prev_wd, "WHERE") == 0)
		COMPLETE_WITH_ATTR(prev2_wd);

/* ... FROM ... */
/* TODO: also include SRF ? */
	else if (strcasecmp(prev_wd, "FROM") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsv);


/* Backslash commands */
/* TODO:  \dc \dd \dl */
	else if (strcmp(prev_wd, "\\connect") == 0 || strcmp(prev_wd, "\\c") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_databases);
	else if (strcmp(prev_wd, "\\d") == 0 || strcmp(prev_wd, "\\d+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tisv);
	else if (strcmp(prev_wd, "\\da") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_aggregates);
	else if (strcmp(prev_wd, "\\dD") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_domains);
	else if (strcmp(prev_wd, "\\df") == 0 || strcmp(prev_wd, "\\df+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_functions);
	else if (strcmp(prev_wd, "\\di") == 0 || strcmp(prev_wd, "\\di+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_indexes);
	else if (strcmp(prev_wd, "\\dn") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_schemas);
	else if (strcmp(prev_wd, "\\dp") == 0 || strcmp(prev_wd, "\\z") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tsv);
	else if (strcmp(prev_wd, "\\ds") == 0 || strcmp(prev_wd, "\\ds+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_sequences);
	else if (strcmp(prev_wd, "\\dS") == 0 || strcmp(prev_wd, "\\dS+") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_system_relations);
	else if (strcmp(prev_wd, "\\dt") == 0 || strcmp(prev_wd, "\\dt+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_tables);
	else if (strcmp(prev_wd, "\\dT") == 0 || strcmp(prev_wd, "\\dT+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_datatypes);
	else if (strcmp(prev_wd, "\\du") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_users);
	else if (strcmp(prev_wd, "\\dv") == 0 || strcmp(prev_wd, "\\dv+") == 0)
		COMPLETE_WITH_SCHEMA_QUERY(Query_for_list_of_views);
	else if (strcmp(prev_wd, "\\encoding") == 0)
		COMPLETE_WITH_QUERY(Query_for_list_of_encodings);
	else if (strcmp(prev_wd, "\\h") == 0 || strcmp(prev_wd, "\\help") == 0)
		COMPLETE_WITH_LIST(sql_commands);
	else if (strcmp(prev_wd, "\\pset") == 0)
	{
		char	   *my_list[] = {"format", "border", "expanded",
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
	 * Finally, we look through the list of "things", such as TABLE, INDEX
	 * and check if that was the previous word. If so, execute the query
	 * to get a list of them.
	 */
	else
	{
		int			i;

		for (i = 0; words_after_create[i].name; i++)
			if (strcasecmp(prev_wd, words_after_create[i].name) == 0)
			{
				if(words_after_create[i].with_schema == WITH_SCHEMA)
					COMPLETE_WITH_SCHEMA_QUERY(words_after_create[i].query);
				else
					COMPLETE_WITH_QUERY(words_after_create[i].query);
				break;
			}
	}


	/*
	 * If we still don't have anything to match we have to fabricate some
	 * sort of default list. If we were to just return NULL, readline
	 * automatically attempts filename completion, and that's usually no
	 * good.
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
   will be free()'d be readline, so you must run it through strdup() or
   something of that sort.
*/

/* This one gives you one from a list of things you can put after CREATE or DROP
   as defined above.
*/
static char *
create_command_generator(const char *text, int state)
{
	static int	list_index,
				string_length;
	char	   *name;

	/* If this is the first time for this completion, init some values */
	if (state == 0)
	{
		list_index = 0;
		string_length = strlen(text);
	}

	/* find something that matches */
	while ((name = words_after_create[list_index++].name))
		if (strncasecmp(name, text, string_length) == 0)
			return xstrdup(name);

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

   See top of file for examples of both kinds of query.
*/

static char *
_complete_from_query(int is_schema_query, const char *text, int state)
{
	static int	list_index,
				string_length;
	static PGresult *result = NULL;
	char		query_buffer[BUF_SIZE];
	const char *item;

	/*
	 * If this is the first time for this completion, we fetch a list of
	 * our "things" from the backend.
	 */
	if (state == 0)
	{
		list_index = 0;
		string_length = strlen(text);

		/* Need to have a query */
		if (completion_charp == NULL)
			return NULL;

		if(is_schema_query)
		{
		  if (snprintf(query_buffer, BUF_SIZE, completion_charp, string_length, text, string_length, text, string_length, text, text,  string_length, text,string_length,text) == -1)
		  {
		      ERROR_QUERY_TOO_LONG;
		      return NULL;
		  }
		}
		else {
		  if (snprintf(query_buffer, BUF_SIZE, completion_charp, string_length, text, completion_info_charp) == -1)
		    {
		      ERROR_QUERY_TOO_LONG;
		      return NULL;
		    }
		}

		result = exec_query(query_buffer);
	}

	/* Find something that matches */
	if (result && PQresultStatus(result) == PGRES_TUPLES_OK)
		while (list_index < PQntuples(result) &&
			   (item = PQgetvalue(result, list_index++, 0)))
			if (strncasecmp(text, item, string_length) == 0)
				return xstrdup(item);

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
	static bool	casesensitive;
	char	   *item;

	/* need to have a list */
#ifdef USE_ASSERT_CHECKING
	assert(completion_charpp);
#endif

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
			return xstrdup(item);
		}

		/* Second pass is case insensitive, don't bother counting matches */
		if (!casesensitive && strncasecmp(text, item, string_length) == 0)
			return xstrdup(item);
	}

	/*
	 * No matches found. If we're not case insensitive already, lets switch
	 * to being case insensitive and try again
	 */
	if (casesensitive && matches == 0)
	{
		casesensitive = false;
		list_index = 0;
		state++;
		return (complete_from_list(text, state));
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

#ifdef USE_ASSERT_CHECKING
	assert(completion_charp);
#endif
	if (state == 0)
		return xstrdup(completion_charp);
	else
		return NULL;
}



/* HELPER FUNCTIONS */


/* Execute a query and report any errors. This should be the preferred way of
   talking to the database in this file.
   Note that the query passed in here must not have a semicolon at the end
   because we need to append LIMIT xxx.
*/
static PGresult *
exec_query(char *query)
{
	PGresult   *result;
	char		query_buffer[BUF_SIZE];

	if (query == NULL || !pset.db || PQstatus(pset.db) != CONNECTION_OK)
		return NULL;
#ifdef USE_ASSERT_CHECKING
	assert(query[strlen(query) - 1] != ';');
#endif

	if (snprintf(query_buffer, BUF_SIZE, "%s LIMIT %d;", query, completion_max_records) == -1)
	{
		ERROR_QUERY_TOO_LONG;
		return NULL;
	}

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



/* Return the word (space delimited) before point. Set skip > 0 to skip that
   many words; e.g. skip=1 finds the word before the previous one.
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
		 * If no end found we return null, because there is no word before
		 * the point
		 */
		if (end == -1)
			return NULL;

		/*
		 * Otherwise we now look for the start. The start is either the
		 * last character before any space going backwards from the end,
		 * or it's simply character 0
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
	s = (char *) malloc(end - start + 2);
	if (!s)
	{
		psql_error("out of memory\n");
		if (!pset.cur_cmd_interactive)
			exit(EXIT_FAILURE);
		else
			return NULL;
	}

	strncpy(s, &rl_line_buffer[start], end - start + 1);
	s[end - start + 1] = '\0';

	return s;
}



#if 0

/*
 * Surround a string with single quotes. This works for both SQL and
 * psql internal. Currently disable because it is reported not to
 * cooperate with certain versions of readline.
 */
char *
quote_file_name(char *text, int match_type, char *quote_pointer)
{
	char	   *s;
	size_t		length;

	(void) quote_pointer;		/* not used */

	length = strlen(text) +(match_type == SINGLE_MATCH ? 3 : 2);
	s = malloc(length);
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
		return xstrdup(text);

	length = strlen(text);
	s = malloc(length - 2 + 1);
	strncpy(s, text +1, length - 2);
	s[length] = '\0';

	return s;
}
#endif   /* 0 */

#endif   /* USE_READLINE */
