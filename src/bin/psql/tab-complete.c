/*-----------
  This file implements a somewhat more sophisticated readline "TAB completion"
  in psql. It is not intended to be AI, to replace learning SQL, or to relieve
  you from thinking about what you're doing. Also it does not always give you
  all the syntactically legal completions, only those that are the most common
  or the ones that the programmer felt most like implementing.

  CAVEAT: Tab completion causes queries to be sent to the backend. The number
  tuples returned gets limited, in most default installations to 101, but if
  you still don't like this prospect, you can turn off tab completion in your
  ~/.inputrc (or else ${INPUTRC}) file so:
  $if psql
  TAB: self-insert
  $endif
  See `man 3 readline` or `info readline` for the full details. Also, hence the

  BUGS:
  * If you split your queries across lines, this whole things gets confused.
    (To fix this, one would have to read psql's query buffer rather than
    readline's line buffer, which would require some major revisions of
    things.)
  * Table or attribute names with spaces in it will equally confuse it.
  * Quotes, parenthesis, and other funny characters are not handled all that
    gracefully.
-------------*/

#include <config.h>
#include <c.h>
#include "tab-complete.h"

#include "input.h"

/* If we don't have this, we might as well forget about the whole thing: */
#ifdef USE_READLINE

#include <stdio.h>
#include <string.h>
#include <ctype.h> /* toupper */
#include <stdlib.h> /* malloc, free */
#ifdef USE_ASSERT_CHECKING
#include <assert.h>
#endif

#include <libpq-fe.h>

#include "common.h"


#define BUF_SIZE 2048
#define ERROR_QUERY_TOO_LONG /* empty */

/* This pointer saves the place where psql stores its own pointer to the
   currently active database connection. This is probably a less than ideal way
   of passing this around, but this way I only had to make minimal changes to
   psql.c. */
static PGconn ** database_connection;
 

/* Forward declaration of functions */
static char ** psql_completion(char *text, int start, int end);
static char * create_command_generator(char *text, int state);
static char * complete_from_query(char *text, int state);
static char * complete_from_const(char *text, int state);
static char * complete_from_list(char *text, int state);

static PGresult * exec_query(char * query);
char * quote_file_name(char *text, int match_type, char * quote_pointer);
//static char * dequote_file_name(char *text, char quote_char);
static char * previous_word(int point, int skip);

/* These variables are used to pass information into the completion functions.
   Realizing that this is the cardinal sin of programming, I don't see a better
   way. */
char * completion_charp;      /* if you need to pass a string */
char ** completion_charpp;    /* if you need to pass a list of strings */
char * completion_info_charp; /* if you need to pass another string */

/* Store how many records from a database query we want to return at most
(implemented via SELECT ... LIMIT xx). */
static int completion_max_records;


static void * xmalloc(size_t length)
{
    void *tmp = malloc(length);
    if (!tmp) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    return tmp;
}


/* Initialize the readline library for our purposes. */
void initialize_readline(PGconn ** conn)
{
    rl_readline_name = "psql";
    rl_attempted_completion_function = psql_completion;

    rl_filename_quoting_function = quote_file_name;
//    rl_filename_dequoting_function = dequote_file_name;
    rl_filename_quote_characters = "qwertyuioplkjhgfdsazxcvbnm";

    rl_special_prefixes = "()'";
    rl_basic_word_break_characters = "\t\n\"'`@$><=;|&{ ";

    completion_max_records = rl_completion_query_items + 1;

    database_connection = conn;
}


/* This is a list of all "things" in Pgsql, which can show up after CREATE or
   DROP; and there is also a query to get a list of them.
   The %s will be replaced by the text entered so far, the %d by it's length.
   If you change the order here or insert things, make sure to also adjust the
   referencing macros below.
*/
typedef struct {
    char * name;
    char * query;
} pgsql_thing_t;

pgsql_thing_t words_after_create[] = {
    { "AGGREGATE", "SELECT distinct aggname FROM pg_aggregate WHERE substr(aggname,1,%d)='%s'" },
    { "DATABASE",  "SELECT datname FROM pg_database WHERE substr(datname,1,%d)='%s'" },
    { "FUNCTION",  "SELECT distinct proname FROM pg_proc WHERE substr(proname,1,%d)='%s'" },
    { "INDEX",     "SELECT relname FROM pg_class WHERE relkind='i' and substr(relname,1,%d)='%s'" },
    { "OPERATOR",  NULL }, /* Querying for this is probably not such a good idea. */
    { "RULE",      "SELECT rulename FROM pg_rules WHERE substr(rulename,1,%d)='%s'" },
    { "SEQUENCE",  "SELECT relname FROM pg_class WHERE relkind='S' and substr(relname,1,%d)='%s'" },
    { "TABLE",     "SELECT relname FROM pg_class WHERE relkind='r' and substr(relname,1,%d)='%s'" },
    { "TEMP",      NULL }, /* for CREATE TEMP TABLE ... */
    { "TRIGGER",   "SELECT tgname FROM pg_trigger WHERE substr(tgname,1,%d)='%s'" },
    { "TYPE",      "SELECT typname FROM pg_type WHERE substr(typname,1,%d)='%s'" },
    { "UNIQUE",    NULL }, /* for CREATE UNIQUE INDEX ... */
    { "USER",      "SELECT usename FROM pg_user WHERE substr(usename,1,%d)='%s'" },
    { "VIEW",      NULL }, /* Telling a view from a table is not the easiest
			      thing in the world, and the solutions I've seen
			      don't really work, so I'll wait on this. */
    { NULL, NULL } /* end of list */
};


/* The query to get a list of tables and a list of indexes, which are used at
   various places. */
#define Query_for_list_of_tables words_after_create[7].query
#define Query_for_list_of_indexes words_after_create[3].query
#define Query_for_list_of_databases words_after_create[1].query
#define Query_for_list_of_attributes "SELECT a.attname FROM pg_attribute a, pg_class c WHERE c.oid = a.attrelid and a.attnum>0 and substr(a.attname,1,%d)='%s' and c.relname='%s'"


/* A couple of macros to ease typing. You can use these to complete the given
   string with
   1) The results from a query you pass it. (Perhaps one of those right above?)
   2) The items from a null-pointer-terminated list.
   3) A string constant
   4) The list of attributes to the given table.
*/
#define COMPLETE_WITH_QUERY(query) \
do { completion_charp = query; matches = completion_matches(text, complete_from_query); } while(0)
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
char ** psql_completion(char *text, int start, int end)
{
    /* This is the variable we'll return. */
    char **matches = NULL;
    /* These are going to contain some scannage of the input line. */
    char *prev_wd, *prev2_wd, *prev3_wd, *prev4_wd;

    static char * sql_commands[] = {
        "ABORT", "ALTER", "BEGIN", "CLOSE", "CLUSTER", "COMMIT", "COPY",
        "CREATE", "DECLARE", "DELETE", "DROP", "EXPLAIN", "FETCH", "GRANT",
        "INSERT", "LISTEN", "LOAD", "LOCK", "MOVE", "NOTIFY", "RESET",
        "REVOKE", "ROLLBACK", "SELECT", "SET", "SHOW", "UNLISTEN", "UPDATE",
        "VACUUM", NULL
    };

    static char * pgsql_variables[] = {
        "Client_Encoding", "Names", "DateStyle", "Server_Encoding", "TimeZone",
        "TRANSACTION", "Cost_Heap", "Cost_Index", "GEQO", "KSQO", "Query_Limit",
        NULL
    };

    static char * backslash_commands[] = {
        "\\connect", "\\copy", "\\d", "\\di", "\\di", "\\ds", "\\dS", "\\dv",
        "\\da", "\\df", "\\do", "\\dt", "\\e", "\\echo", "\\g", "\\h", "\\i", "\\l",
        "\\lo_import", "\\lo_export", "\\lo_list", "\\lo_unlink",
        "\\o", "\\p", "\\pset", "\\q", "\\qecho", "\\r", "\\set", "\\t", "\\x",
        "\\w", "\\z", "\\!", NULL
    };

    (void)end; /* not used */

    rl_filename_quoting_desired = 1;

    /* Clear a few things. */
    completion_charp = NULL;
    completion_charpp = NULL;
    completion_info_charp = NULL;

    /* Scan the input line before our current position for the last four words.
       According to those we'll make some smart decisions on what the user is
       probably intending to type.
       TODO: Use strtokx() to do this.
    */
    prev_wd = previous_word(start,0);
    prev2_wd = previous_word(start,1);
    prev3_wd = previous_word(start,2);
    prev4_wd = previous_word(start,3);

    /* If a backslash command was started, continue */
    if (text[0]=='\\')
        COMPLETE_WITH_LIST(backslash_commands);

    /* If no previous word, suggest one of the basic sql commands */
    else if (!prev_wd)
        COMPLETE_WITH_LIST(sql_commands);

/* CREATE or DROP */
    /* complete with something you can create or drop */
    else if( strcasecmp(prev_wd, "CREATE") == 0 || strcasecmp(prev_wd, "DROP") == 0 )
        matches = completion_matches(text, create_command_generator);

/* ALTER */
    /* complete with what you can alter (TABLE or USER) */
    else if( strcasecmp(prev_wd, "ALTER") == 0 ) {
        char * list_ALTER[] = { "TABLE", "USER", NULL };
        COMPLETE_WITH_LIST(list_ALTER);
    }
    /* If we detect ALTER TABLE <name>, suggest either "ADD" or "RENAME" */
    else if( strcasecmp(prev3_wd, "ALTER")==0 && strcasecmp(prev2_wd, "TABLE")==0 ) {
        char * list_ALTER2[] = { "ADD", "RENAME", NULL };
        COMPLETE_WITH_LIST(list_ALTER2);
    }
    /* If we have TABLE <sth> ADD|RENAME, provide list of columns */
    else if( strcasecmp(prev3_wd, "TABLE")==0 &&
             (strcasecmp(prev_wd,"ADD")==0 || strcasecmp(prev_wd,"RENAME")==0) )
        COMPLETE_WITH_ATTR(prev2_wd);

/* CLUSTER */
    /* If the previous word is CLUSTER, produce list of indexes. */
    else if( strcasecmp(prev_wd, "CLUSTER") == 0 )
        COMPLETE_WITH_QUERY(Query_for_list_of_indexes);
    /* If we have CLUSTER <sth>, then add "ON" */
    else if( strcasecmp(prev2_wd, "CLUSTER") == 0 )
        COMPLETE_WITH_CONST("ON");
    /* If we have CLUSTER <sth> ON, then add the correct tablename as well. */
    else if ( strcasecmp(prev3_wd, "CLUSTER")==0 && strcasecmp(prev_wd, "ON")==0 ) {
        char query_buffer[BUF_SIZE]; /* Some room to build queries. */
        if(snprintf(query_buffer, BUF_SIZE,
                    "SELECT c1.relname FROM pg_class c1, pg_class c2, pg_index i WHERE c1.oid=i.indrelid and i.indexrelid=c2.oid and c2.relname='%s'",
                    prev2_wd) == -1)
            ERROR_QUERY_TOO_LONG;
        else
            COMPLETE_WITH_QUERY(query_buffer);
    }

/* COPY */
    /* If we have COPY [BINARY] (which you'd have to type yourself), offer list of tables
       (Also cover the analogous backslash command) */
    else if( strcasecmp(prev_wd, "COPY")==0 ||
             strcasecmp(prev_wd, "\\copy")==0 ||
             (strcasecmp(prev2_wd,"COPY")==0 && strcasecmp(prev_wd,"BINARY")==0)  )
        COMPLETE_WITH_QUERY(Query_for_list_of_tables);
    /* If we have COPY|BINARY <sth>, complete it with "TO" or "FROM" */
    else if( strcasecmp(prev2_wd, "COPY")==0 ||
             strcasecmp(prev2_wd, "\\copy")==0 ||
             strcasecmp(prev2_wd, "BINARY")==0 ) {
        char * list_FROMTO[] = { "FROM", "TO", NULL };
        COMPLETE_WITH_LIST(list_FROMTO);
    }

/* CREATE INDEX */
    /* First off we complete CREATE UNIQUE with "INDEX" */
    else if( strcasecmp(prev2_wd, "CREATE")==0 && strcasecmp(prev_wd, "UNIQUE")==0 )
        COMPLETE_WITH_CONST("INDEX");
    /* If we have CREATE|UNIQUE INDEX <sth>, then add "ON" */
    else if( strcasecmp(prev2_wd, "INDEX") == 0 &&
             (strcasecmp(prev3_wd,"CREATE")==0 || strcasecmp(prev3_wd,"UNIQUE")==0) )
        COMPLETE_WITH_CONST("ON");
    /* Complete ... INDEX <name> ON with a list of tables  */
    else if( (strcasecmp(prev3_wd, "INDEX")==0 && strcasecmp(prev_wd, "ON")==0) || (0) )
        COMPLETE_WITH_QUERY(Query_for_list_of_tables);
    /* Complete INDEX <name> ON <table> with a list of table columns (which should really be in parens) */
    else if( (strcasecmp(prev4_wd, "INDEX")==0 && strcasecmp(prev2_wd, "ON")==0) )
        COMPLETE_WITH_ATTR(prev_wd);
    /* same if you put in USING */
    else if ((strcasecmp(prev4_wd,"ON")==0 && strcasecmp(prev2_wd,"USING")==0) )
        COMPLETE_WITH_ATTR(prev3_wd);
    /* Complete USING with an index method */
    else if( strcasecmp(prev_wd, "USING")==0 ) {
        char * index_mth[] = { "BTREE", "RTREE", "HASH", NULL };
        COMPLETE_WITH_LIST(index_mth);
    }
    
/* CREATE RULE */
    /* Complete "CREATE RULE <sth>" with "AS" */
    else if( strcasecmp(prev3_wd,"CREATE")==0 && strcasecmp(prev2_wd,"RULE")==0 )
        COMPLETE_WITH_CONST("AS");
    /* Complete "CREATE RULE <sth> AS with "ON" */
    else if( strcasecmp(prev4_wd,"CREATE")==0 &&
             strcasecmp(prev3_wd,"RULE")==0 &&
             strcasecmp(prev_wd,"AS")==0 )
        COMPLETE_WITH_CONST("ON");
    /* Complete "RULE * AS ON" with SELECT|UPDATE|DELETE|INSERT */
    else if( strcasecmp(prev4_wd,"RULE")==0 &&
             strcasecmp(prev2_wd,"AS")==0 &&
             strcasecmp(prev_wd,"ON")==0 ) {
        char * rule_events[] = { "SELECT", "UPDATE", "INSERT", "DELETE", NULL };
        COMPLETE_WITH_LIST(rule_events);
    }
    /* Complete "AS ON <sth with a 'T' :)>" with a "TO" */
    else if( strcasecmp(prev3_wd,"AS")==0 &&
             strcasecmp(prev2_wd,"ON")==0 &&
             (toupper(prev_wd[4])=='T' || toupper(prev_wd[5])=='T') )
        COMPLETE_WITH_CONST("TO");
    /* Complete "AS ON <sth> TO" with a table name */
    else if( strcasecmp(prev4_wd,"AS")==0 &&
             strcasecmp(prev3_wd,"ON")==0 &&
             strcasecmp(prev_wd,"TO")==0 )
        COMPLETE_WITH_QUERY(Query_for_list_of_tables);

/* CREATE TABLE */
    /* Complete CREATE TEMP with "TABLE" */
    else if( strcasecmp(prev2_wd, "CREATE")==0 && strcasecmp(prev_wd, "TEMP")==0 )
        COMPLETE_WITH_CONST("TABLE");

/* CREATE TRIGGER */
    /* is on the agenda . . . */

/* CREATE VIEW */
    /* Complete "CREATE VIEW <name>" with "AS" */
    else if( strcasecmp(prev3_wd,"CREATE")==0 && strcasecmp(prev2_wd,"VIEW")==0 )
        COMPLETE_WITH_CONST("AS");
    /* Complete "CREATE VIEW <sth> AS with "SELECT" */
    else if( strcasecmp(prev4_wd,"CREATE")==0 &&
             strcasecmp(prev3_wd,"VIEW")==0 &&
             strcasecmp(prev_wd,"AS")==0 )
        COMPLETE_WITH_CONST("SELECT");

/* DELETE */
    /* Complete DELETE with FROM (only if the word before that is not "ON" (cf.
       rules) or "BEFORE" or "AFTER" (cf. triggers) ) */
    else if( strcasecmp(prev_wd,"DELETE")==0 &&
             !(strcasecmp(prev2_wd,"ON")==0 ||
               strcasecmp(prev2_wd,"BEFORE")==0 ||
               strcasecmp(prev2_wd,"AFTER")==0) )
        COMPLETE_WITH_CONST("FROM");
    /* Complete DELETE FROM with a list of tables */
    else if( strcasecmp(prev2_wd,"DELETE")==0 && strcasecmp(prev_wd,"FROM")==0 )
        COMPLETE_WITH_QUERY(Query_for_list_of_tables);
    /* Complete DELETE FROM <table> with "WHERE" (perhaps a safe idea?) */
    else if( strcasecmp(prev3_wd,"DELETE")==0 && strcasecmp(prev2_wd,"FROM")==0 )
        COMPLETE_WITH_CONST("WHERE");

/* EXPLAIN */
    /* Complete EXPLAIN [VERBOSE] (which you'd have to type yourself) with the list of SQL commands */
    else if( strcasecmp(prev_wd,"EXPLAIN")==0 ||
             (strcasecmp(prev2_wd,"EXPLAIN")==0 && strcasecmp(prev_wd,"VERBOSE")==0) )
        COMPLETE_WITH_LIST(sql_commands);

/* FETCH && MOVE */
    /* Complete FETCH with one of FORWARD, BACKWARD, RELATIVE */
    else if( strcasecmp(prev_wd,"FETCH")==0 || strcasecmp(prev_wd,"MOVE")==0 ) {
        char * list_FETCH1[] = { "FORWARD", "BACKWARD", "RELATIVE", NULL };
        COMPLETE_WITH_LIST(list_FETCH1);
    }
    /* Complete FETCH <sth> with one of ALL, NEXT, PRIOR */
    else if( strcasecmp(prev2_wd,"FETCH")==0 || strcasecmp(prev2_wd,"MOVE")==0 ) {
        char * list_FETCH2[] = { "ALL", "NEXT", "PRIOR", NULL };
        COMPLETE_WITH_LIST(list_FETCH2);
    }
    /* Complete FETCH <sth1> <sth2> with "FROM" or "TO".
       (Is there a difference? If not, remove one.) */
    else if( strcasecmp(prev3_wd,"FETCH")==0 || strcasecmp(prev3_wd,"MOVE")==0 ) {
        char * list_FROMTO[] = { "FROM", "TO", NULL };
        COMPLETE_WITH_LIST(list_FROMTO);
    }

/* GRANT && REVOKE*/
    /* Complete GRANT/REVOKE with a list of privileges */
    else if( strcasecmp(prev_wd,"GRANT")==0 || strcasecmp(prev_wd,"REVOKE")==0 ) {
        char * list_privileg[] = { "SELECT", "INSERT", "UPDATE", "DELETE", "RULE", "ALL", NULL };
        COMPLETE_WITH_LIST(list_privileg);
    }
    /* Complete GRANT/REVOKE <sth> with "ON" */
    else if( strcasecmp(prev2_wd,"GRANT")==0 || strcasecmp(prev2_wd,"REVOKE")==0 )
        COMPLETE_WITH_CONST("ON");
    /* Complete GRANT/REVOKE <sth> ON with a list of tables, views, sequences, and indexes */
    else if( (strcasecmp(prev3_wd,"GRANT")==0 || strcasecmp(prev3_wd,"REVOKE")==0) &&
             strcasecmp(prev_wd,"ON")==0 )
        COMPLETE_WITH_QUERY("SELECT relname FROM pg_class WHERE relkind in ('r','i','s') and substr(relname,1,%d)='%s'");
    /* Complete "GRANT * ON * " with "TO" */
    else if( strcasecmp(prev4_wd,"GRANT")==0 && strcasecmp(prev2_wd,"ON")==0 )
        COMPLETE_WITH_CONST("TO");
    /* Complete "REVOKE * ON * " with "FROM" */
    else if( strcasecmp(prev4_wd,"REVOKE")==0 && strcasecmp(prev2_wd,"ON")==0 )
        COMPLETE_WITH_CONST("FROM");
    /* TODO: to complete with user name we need prev5_wd -- wait for a more general solution there */

/* INSERT */
    /* Complete INSERT with "INTO" */
    else if( strcasecmp(prev_wd,"INSERT")==0 )
        COMPLETE_WITH_CONST("INTO");
    /* Complete INSERT INTO with table names */
    else if( strcasecmp(prev2_wd, "INSERT")==0 && strcasecmp(prev_wd,"INTO")==0 )
      	COMPLETE_WITH_QUERY(Query_for_list_of_tables);
    /* Complete INSERT INTO <table> with "VALUES" or "SELECT" */
    else if( strcasecmp(prev3_wd, "INSERT")==0 && strcasecmp(prev2_wd,"INTO")==0 ) {
        char * list_INSERT[] = { "SELECT", "VALUES", NULL };
        COMPLETE_WITH_LIST(list_INSERT);
    }
    /* Insert an open parenthesis after "VALUES" */
    else if( strcasecmp(prev_wd,"VALUES")==0 )
        COMPLETE_WITH_CONST("(");

/* LOCK */
    /* Complete with list of tables */
    else if( strcasecmp(prev_wd, "LOCK") == 0 )
      	COMPLETE_WITH_QUERY(Query_for_list_of_tables);
    /* (If you want more with LOCK, you better think about it yourself.) */

/* SELECT */
    /* naah . . . */

/* SET, RESET, SHOW */
    /* Complete with a variable name */
    else if( (strcasecmp(prev_wd,"SET")==0 && strcasecmp(prev3_wd,"UPDATE")!=0) ||
             strcasecmp(prev_wd,"RESET")==0 ||
             strcasecmp(prev_wd,"SHOW")==0 )
        COMPLETE_WITH_LIST(pgsql_variables);
    /* Complete "SET TRANSACTION ISOLOLATION LEVEL" */
    else if( strcasecmp(prev2_wd,"SET")==0 && strcasecmp(prev_wd,"TRANSACTION")==0 )
        COMPLETE_WITH_CONST("ISOLATION");
    else if( strcasecmp(prev3_wd,"SET")==0 &&
             strcasecmp(prev2_wd,"TRANSACTION")==0 &&
             strcasecmp(prev_wd,"ISOLATION")==0 )
        COMPLETE_WITH_CONST("LEVEL");
    else if( strcasecmp(prev4_wd,"SET")==0 &&
             strcasecmp(prev3_wd,"TRANSACTION")==0 &&
             strcasecmp(prev2_wd,"ISOLATION")==0 &&
             strcasecmp(prev_wd,"LEVEL")==0 ) {
        char * my_list[] = {"READ","SERIALIZED",NULL};
        COMPLETE_WITH_LIST(my_list);
    }
    else if( strcasecmp(prev4_wd,"TRANSACTION")==0 &&
             strcasecmp(prev3_wd,"ISOLATION")==0 &&
             strcasecmp(prev2_wd,"LEVEL")==0 &&
             strcasecmp(prev_wd,"READ")==0 )
        COMPLETE_WITH_CONST("COMMITTED");
    /* Complete SET <var> with "TO" */
    else if( strcasecmp(prev2_wd,"SET")==0 &&
             strcasecmp(prev4_wd,"UPDATE")!=0 )
        COMPLETE_WITH_CONST("TO");
    /* Suggest possible variable values */
    else if( strcasecmp(prev3_wd,"SET")==0 &&
             (strcasecmp(prev_wd,"TO")==0 || strcmp(prev_wd,"=")==0) ) {
        if ( strcasecmp(prev2_wd,"DateStyle")==0 ) {
            char * my_list[] = {"'ISO'", "'SQL'", "'Postgres'", "'European'", "'NonEuropean'", "'German'", "DEFAULT", NULL};
            COMPLETE_WITH_LIST(my_list);
        }
        else if( strcasecmp(prev2_wd,"GEQO")==0 || strcasecmp(prev2_wd,"KSQO")==0 ) {
            char * my_list[] = {"ON", "OFF", "DEFAULT", NULL};
            COMPLETE_WITH_LIST(my_list);
        }
        else {
            char * my_list[] = {"DEFAULT", NULL};
            COMPLETE_WITH_LIST(my_list);
        }
    }

/* UPDATE */
    /* If prev. word is UPDATE suggest a list of tables */
    else if( strcasecmp(prev_wd, "UPDATE") == 0 )
      	COMPLETE_WITH_QUERY(Query_for_list_of_tables);
    /* Complete UPDATE <table> with "SET" */
    else if( strcasecmp(prev2_wd, "UPDATE") == 0 )
        COMPLETE_WITH_CONST("SET");
    /* If the previous word is SET (and it wasn't caught above as the _first_
       word) the word before it was (hopefully) a table name and we'll now make
       a list of attributes. */
    else if( strcasecmp(prev_wd, "SET") == 0 )
        COMPLETE_WITH_ATTR(prev2_wd);

/* VACUUM */
    else if( strcasecmp(prev_wd, "VACUUM") == 0 )
        COMPLETE_WITH_QUERY("SELECT relname FROM pg_class WHERE relkind='r' and substr(relname,1,%d)='%s' UNION SELECT 'ANALYZE'::text");
    else if( strcasecmp(prev2_wd, "VACUUM")==0 && strcasecmp(prev_wd, "ANALYZE")==0 )
      	COMPLETE_WITH_QUERY(Query_for_list_of_tables);


/* Backslash commands */
    else if (strcmp(prev_wd, "\\connect")==0 || strcmp(prev_wd, "\\c")==0)
        COMPLETE_WITH_QUERY(Query_for_list_of_databases);
    else if (strcmp(prev_wd, "\\d")==0)
        COMPLETE_WITH_QUERY(Query_for_list_of_tables);
    else if (strcmp(prev_wd, "\\h")==0 || strcmp(prev_wd, "\\help")==0)
        COMPLETE_WITH_LIST(sql_commands);
    else if (strcmp(prev_wd, "\\pset")==0) {
        char * my_list[] = { "format", "border", "expanded", "null", "fieldsep",
                             "tuples_only", "title", "tableattr", "pager" };
        COMPLETE_WITH_LIST(my_list);
    }
    else if( strcmp(prev_wd, "\\e")==0 || strcmp(prev_wd, "\\edit")==0 ||
             strcmp(prev_wd, "\\g")==0 ||
             strcmp(prev_wd, "\\i")==0 || strcmp(prev_wd, "\\include")==0 ||
             strcmp(prev_wd, "\\o")==0 || strcmp(prev_wd, "\\out")==0 ||
             strcmp(prev_wd, "\\s")==0 ||
             strcmp(prev_wd, "\\w")==0 || strcmp(prev_wd, "\\write")==0
        ) {
        matches = completion_matches(text, filename_completion_function);
    }


    /* Finally, we look through the list of "things", such as TABLE, INDEX and
       check if that was the previous word. If so, execute the query to get a
       list of them. */
    else {
        int i;
        for(i=0; words_after_create[i].name; i++)
            if ( strcasecmp(prev_wd, words_after_create[i].name) == 0 ) {
                COMPLETE_WITH_QUERY(words_after_create[i].query);
                break;
            }
    }


    /* If we still don't have anything to match we have to fabricate some sort
       of default list. If we were to just return NULL, readline automatically
       attempts filename completion, and that's usually no good. */
    if (matches == NULL) {
        char * my_list[] = { "", "", NULL };
        COMPLETE_WITH_LIST(my_list);
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
char * create_command_generator(char *text, int state)
{
    static int list_index, string_length;
    char *name;

    /* If this is the first time for this completion, init some values */
    if (state == 0) {
        list_index = 0;
        string_length = strlen(text);
    }

    /* find something that matches */
    while ( (name = words_after_create[list_index++].name) )
        if ( strncasecmp(name, text, string_length) == 0 )
            return xstrdup(name);

    /* if nothing matches, return NULL */
    return NULL;
}


/* This creates a list of matching things, according to a query pointed to
   by completion_charp. The query needs to have a %d and a %s in it, which will
   be replaced by the string length of the text and the text itself. See some
   example queries at the top.
   The query may also have another %s in it, which will be replaced by the value
   of completion_info_charp.
   Ordinarily this would be used to get a list of matching tables or functions,
   etc.
*/
char * complete_from_query(char *text, int state)
{
    static int list_index, string_length;
    static PGresult *result = NULL;
    char query_buffer[BUF_SIZE];
    const char * item;

    /* If this ist the first time for this completion, we fetch a list of our
       "things" from the backend. */
    if (state == 0) {
        list_index = 0;
        string_length = strlen(text);

        /* Need to have a query */
        if (completion_charp == NULL) return NULL;

        if (snprintf(query_buffer, BUF_SIZE, completion_charp, string_length, text, completion_info_charp) == -1) {
            ERROR_QUERY_TOO_LONG;
            return NULL;
        }

        result = exec_query(query_buffer);
    }

    /* Find something that matches */
    if( result && PQresultStatus(result) == PGRES_TUPLES_OK )
        while( list_index < PQntuples(result) && (item = PQgetvalue(result, list_index++, 0)) )
            if ( strncasecmp(text, item, string_length) == 0)
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
char * complete_from_list(char *text, int state) {
    static int string_length, list_index;
    char * item;

    /* need to have a list */
#ifdef USE_ASSERT_CHECKING
    assert(completion_charpp);
#endif

    /* Initialization */
    if (state == 0) {
        list_index = 0;
        string_length = strlen(text);
    }

    while( (item = completion_charpp[list_index++]) )
        if ( strncasecmp(text,item,string_length) == 0 )
            return xstrdup(item);

    /* If no more matches, return null. */
    return NULL;
}


/* This function returns one fixed string the first time even if it doesn't
   match what's there, and nothing the second time. This should be used if there
   is only one possibility that can appear at a certain spot, so misspellings
   will be overwritten.
   The string to be passed must be in completion_charp.
*/
char * complete_from_const(char *text, int state)
{
    (void)text; /* We don't care about what was entered already. */
#ifdef USE_ASSERT_CHECKING
    assert(completion_charp);
#endif
    if (state==0)
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
PGresult * exec_query(char * query)
{
    PGresult * result;
    char query_buffer[BUF_SIZE];

    if (query == NULL || PQstatus(*database_connection) != CONNECTION_OK)
        return NULL;
#ifdef USE_ASSERT_CHECKING
    assert( query[strlen(query)-1] != ';' );
#endif

    if( snprintf(query_buffer, BUF_SIZE, "%s LIMIT %d;", query, completion_max_records) == -1 ) {
        ERROR_QUERY_TOO_LONG;
        return NULL;
    }

    result = PQexec(*database_connection, query);

    if (result != NULL && PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "\nThe completion query \"%s\" failed thus: %s\n",
                query, PQresStatus(PQresultStatus(result)));
        PQclear(result);
        result = NULL;
    }
    
    return result;
}



/* Return the word (space delimited) before point. Set skip > 0 to skip that
   many words; e.g. skip=1 finds the word before the previous one.
   TODO: Take account of quotes. (Right now, if you table names contain spaces
   you're screwed.)
*/
char * previous_word(int point, int skip) {
    int i, start=0, end=-1;
    char * s;

    while (skip-- >=0) {
        /* first we look for a space before the current word */
        for(i=point; i>=0; i--)
            if (rl_line_buffer[i] == ' ')
                break;

        /* now find the first non-space which then constitutes the end */
        for(; i>=0; i--)
            if (rl_line_buffer[i] != ' ') {
                end = i;
                break;
            }

        /* If no end found we return null, because there is no word before the point */
        if (end == -1)
            return NULL;

        /* Otherwise we now look for the start. The start is either the last
           character before any space going backwards from the end, or it's
           simply character 0 */
        for (start=end; start>0; start--)
            if (rl_line_buffer[start-1] == ' ')
                break;

        point=start;
    }

    /* make a copy */
    s = (char *)xmalloc(end-start+2);
    strncpy(s, &rl_line_buffer[start], end-start+1);
    s[end-start+1] = '\0';

    return s;
}


/* Surround a string with single quotes. This works for both SQL and
   psql internal. Doesn't work so well yet.
*/
char * quote_file_name(char *text, int match_type, char * quote_pointer)
{
    char *s;
    size_t length;

    (void)quote_pointer; /* not used */

    length = strlen(text) + ( match_type==SINGLE_MATCH ? 3 : 2 );
    s = xmalloc(length);
    s[0] = '\'';
    strcpy(s+1, text);
    if (match_type==SINGLE_MATCH)
        s[length-2] = '\'';
    s[length-1] = '\0';
    return s;
}


#ifdef NOT_USED
static char * dequote_file_name(char *text, char quote_char)
{
    char *s;
    size_t length;

    if (!quote_char)
        return xstrdup(text);

    length = strlen(text);
    s = xmalloc(length-2+1);
    strncpy(s, text+1, length-2);
    s[length] = '\0';

    return s;
}
#endif

#endif /* USE_READLINE */
