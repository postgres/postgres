/*--------------------------------------------------------------------
 * guc.c
 *
 * Support for grand unified configuration scheme, including SET
 * command, configuration file, and command line options.
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/misc/guc.c,v 1.25 2000/11/30 01:47:32 vadim Exp $
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *--------------------------------------------------------------------
 */

#include "postgres.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <unistd.h>

#include "utils/guc.h"

#include "commands/async.h"
#include "libpq/auth.h"
#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "parser/parse_expr.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"


/* XXX should be in a header file */
extern bool Log_connections;

extern int CheckPointTimeout;
extern int XLOGbuffers;
extern int XLOG_DEBUG;
extern int CommitDelay;

#ifdef ENABLE_SYSLOG
extern char *Syslog_facility;
extern char *Syslog_ident;
bool check_facility(const char *facility);
#endif

/*
 * Debugging options
 */
#ifdef USE_ASSERT_CHECKING
bool assert_enabled         = true;
#endif
bool Debug_print_query      = false;
bool Debug_print_plan       = false;
bool Debug_print_parse      = false;
bool Debug_print_rewritten  = false;
bool Debug_pretty_print     = false;

bool Show_parser_stats      = false;
bool Show_planner_stats     = false;
bool Show_executor_stats    = false;
bool Show_query_stats       = false; /* this is sort of all three above together */
bool Show_btree_build_stats = false;

bool SQL_inheritance        = true;

#ifndef PG_KRB_SRVTAB
# define PG_KRB_SRVTAB ""
#endif



enum config_type
{
    PGC_NONE = 0,
    PGC_BOOL,
    PGC_INT,
    PGC_REAL,
    PGC_STRING
};


struct config_generic
{
    const char *name;
    GucContext  context;
    void       *variable;
};


struct config_bool
{
    const char *name;
    GucContext  context;
    bool       *variable;
    bool        default_val;
};


struct config_int
{
    const char *name;
    GucContext  context;
    int        *variable;
    int         default_val;
    int         min;
    int         max;
};


struct config_real
{
    const char *name;
    GucContext  context;
    double     *variable;
    double      default_val;
    double      min;
    double      max;
};

/*
 * String value options are allocated with strdup, not with the
 * pstrdup/palloc mechanisms. That is because configuration settings
 * are already in place before the memory subsystem is up. It would
 * perhaps be an idea to change that sometime.
 */
struct config_string
{
    const char *name;
    GucContext  context;
    char      **variable;
    const char *default_val;
    bool       (*parse_hook)(const char *);
};


/*
 * TO ADD AN OPTION:
 *
 * 1. Declare a global variable of type bool, int, double, or char*
 * and make use of it.
 *
 * 2. Decide at what times it's safe to set the option. See guc.h for
 * details.
 *
 * 3. Decide on a name, a default value, upper and lower bounds (if
 * applicable), etc.
 *
 * 4. Add a record below.
 *
 * 5. Don't forget to document that option.
 */


/******** option names follow ********/

static struct config_bool
ConfigureNamesBool[] =
{
	{"enable_seqscan",          PGC_USERSET,    &enable_seqscan,        true},
	{"enable_indexscan",        PGC_USERSET,    &enable_indexscan,      true},
	{"enable_tidscan",          PGC_USERSET,    &enable_tidscan,        true},
	{"enable_sort",             PGC_USERSET,    &enable_sort,           true},
	{"enable_nestloop",         PGC_USERSET,    &enable_nestloop,       true},
	{"enable_mergejoin",        PGC_USERSET,    &enable_mergejoin,      true},
	{"enable_hashjoin",         PGC_USERSET,    &enable_hashjoin,       true},

	{"ksqo",                    PGC_USERSET,    &_use_keyset_query_optimizer, false},
	{"geqo",                    PGC_USERSET,    &enable_geqo,           true},

	{"tcpip_socket",            PGC_POSTMASTER, &NetServer,             false},
	{"ssl",                     PGC_POSTMASTER, &EnableSSL,             false},
	{"fsync",                   PGC_USERSET,    &enableFsync,           true},
	{"silent_mode",             PGC_POSTMASTER, &SilentMode,            false},

	{"log_connections",         PGC_SIGHUP,     &Log_connections,       false},
	{"log_timestamp",           PGC_SIGHUP,     &Log_timestamp,         false},
	{"log_pid",                 PGC_SIGHUP,     &Log_pid,               false},

#ifdef USE_ASSERT_CHECKING
	{"debug_assertions",        PGC_USERSET,    &assert_enabled,        true},
#endif

	{"debug_print_query",       PGC_USERSET,    &Debug_print_query,     false},
	{"debug_print_parse",       PGC_USERSET,    &Debug_print_parse,     false},
	{"debug_print_rewritten",   PGC_USERSET,    &Debug_print_rewritten, false},
	{"debug_print_plan",        PGC_USERSET,    &Debug_print_plan,      false},
	{"debug_pretty_print",      PGC_USERSET,    &Debug_pretty_print,    false},

	{"show_parser_stats",       PGC_USERSET,    &Show_parser_stats,     false},
	{"show_planner_stats",      PGC_USERSET,    &Show_planner_stats,    false},
	{"show_executor_stats",     PGC_USERSET,    &Show_executor_stats,   false},
	{"show_query_stats",        PGC_USERSET,    &Show_query_stats,      false},
#ifdef BTREE_BUILD_STATS
	{"show_btree_build_stats",  PGC_SUSET,      &Show_btree_build_stats, false},
#endif

	{"trace_notify",            PGC_USERSET,    &Trace_notify,          false},

#ifdef LOCK_DEBUG
	{"trace_locks",             PGC_SUSET,      &Trace_locks,           false},
	{"trace_userlocks",         PGC_SUSET,      &Trace_userlocks,       false},
	{"trace_spinlocks",         PGC_SUSET,      &Trace_spinlocks,       false},
	{"debug_deadlocks",         PGC_SUSET,      &Debug_deadlocks,       false},
#endif

	{"hostlookup",              PGC_SIGHUP,     &HostnameLookup,        false},
	{"showportnumber",          PGC_SIGHUP,     &ShowPortNumber,        false},

	{"sql_inheritance",         PGC_USERSET,    &SQL_inheritance,       true},

	{NULL, 0, NULL, false}
};


static struct config_int
ConfigureNamesInt[] =
{
	{"geqo_rels",               PGC_USERSET,            &geqo_rels,
	 DEFAULT_GEQO_RELS, 2, INT_MAX},
	{"geqo_pool_size",          PGC_USERSET,            &Geqo_pool_size,
	 DEFAULT_GEQO_POOL_SIZE, 0, MAX_GEQO_POOL_SIZE},
	{"geqo_effort",             PGC_USERSET,            &Geqo_effort,
	 1, 1, INT_MAX},
	{"geqo_generations",        PGC_USERSET,            &Geqo_generations,
	 0, 0, INT_MAX},
	{"geqo_random_seed",        PGC_USERSET,            &Geqo_random_seed,
	 -1, INT_MIN, INT_MAX},

	{"deadlock_timeout",        PGC_POSTMASTER,         &DeadlockTimeout,
	 1000, 0, INT_MAX},

#ifdef ENABLE_SYSLOG
	{"syslog",                  PGC_SIGHUP,             &Use_syslog,
	 0, 0, 2},
#endif

	/*
	 * Note: There is some postprocessing done in PostmasterMain() to
	 * make sure the buffers are at least twice the number of
	 * backends, so the constraints here are partially unused.
	 */
	{"max_connections",         PGC_POSTMASTER,         &MaxBackends,
	 DEF_MAXBACKENDS, 1, MAXBACKENDS},
	{"shared_buffers",          PGC_POSTMASTER,         &NBuffers,
	 DEF_NBUFFERS, 16, INT_MAX},
	{"port",                    PGC_POSTMASTER,         &PostPortNumber,
	 DEF_PGPORT, 1, 65535},

	{"sort_mem",                PGC_USERSET,            &SortMem,
	 512, 1, INT_MAX},

	{"debug_level",             PGC_USERSET,            &DebugLvl,
	 0, 0, 16},

#ifdef LOCK_DEBUG
	{"trace_lock_oidmin",       PGC_SUSET,              &Trace_lock_oidmin,
	 BootstrapObjectIdData, 1, INT_MAX},
	{"trace_lock_table",        PGC_SUSET,              &Trace_lock_table,
	 0, 0, INT_MAX},
#endif
	{"max_expr_depth",          PGC_USERSET,            &max_expr_depth,
	 DEFAULT_MAX_EXPR_DEPTH, 10, INT_MAX},

	{"unix_socket_permissions", PGC_POSTMASTER,         &Unix_socket_permissions,
	 0777, 0000, 0777},

	{"checkpoint_timeout",	PGC_POSTMASTER,			&CheckPointTimeout,
	 300, 30, 1800},

	{"wal_buffers",			PGC_POSTMASTER,			&XLOGbuffers,
	 8, 4, INT_MAX},

	{"wal_debug",			PGC_SUSET,				&XLOG_DEBUG,
	 0, 0, 16},

	{"commit_delay",		PGC_USERSET,			&CommitDelay,
	 5, 0, 1000},

    {NULL, 0, NULL, 0, 0, 0}
};


static struct config_real
ConfigureNamesReal[] =
{
    {"effective_cache_size",      PGC_USERSET,          &effective_cache_size,
     DEFAULT_EFFECTIVE_CACHE_SIZE, 0, DBL_MAX},
    {"random_page_cost",          PGC_USERSET,          &random_page_cost,
     DEFAULT_RANDOM_PAGE_COST, 0, DBL_MAX},
    {"cpu_tuple_cost",            PGC_USERSET,          &cpu_tuple_cost,
     DEFAULT_CPU_TUPLE_COST, 0, DBL_MAX},
    {"cpu_index_tuple_cost",      PGC_USERSET,          &cpu_index_tuple_cost,
     DEFAULT_CPU_INDEX_TUPLE_COST, 0, DBL_MAX},
    {"cpu_operator_cost",         PGC_USERSET,          &cpu_operator_cost,
     DEFAULT_CPU_OPERATOR_COST, 0, DBL_MAX},

    {"geqo_selection_bias",       PGC_USERSET,          &Geqo_selection_bias,
     DEFAULT_GEQO_SELECTION_BIAS,   MIN_GEQO_SELECTION_BIAS, MAX_GEQO_SELECTION_BIAS},

    {NULL, 0, NULL, 0.0, 0.0, 0.0}
};


static struct config_string
ConfigureNamesString[] =
{
	{"krb_server_keyfile",        PGC_POSTMASTER,       &pg_krb_server_keyfile,
	 PG_KRB_SRVTAB, NULL},

	{"unix_socket_group",         PGC_POSTMASTER,       &Unix_socket_group,
	 "", NULL},

#ifdef ENABLE_SYSLOG
	{"syslog_facility",           PGC_POSTMASTER,	    &Syslog_facility, 
	"LOCAL0", check_facility},	 
	{"syslog_ident",              PGC_POSTMASTER,	    &Syslog_ident, 
	"postgres", NULL},	 
#endif

	{"unix_socket_directory",	  PGC_POSTMASTER,       &UnixSocketDir,
	 "", NULL},

	{"virtual_host",			  PGC_POSTMASTER,		&VirtualHost,
	 "", NULL},

	{NULL, 0, NULL, NULL, NULL}
};

/******** end of options list ********/



/*
 * Look up option NAME. If it exists, return it's data type, else
 * PGC_NONE (zero). If record is not NULL, store the description of
 * the option there.
 */
static enum config_type
find_option(const char * name, struct config_generic ** record)
{
    int i;

    Assert(name);

    for (i = 0; ConfigureNamesBool[i].name; i++)
        if (strcasecmp(ConfigureNamesBool[i].name, name)==0)
        {
            if (record)
                *record = (struct config_generic *)&ConfigureNamesBool[i];
            return PGC_BOOL;
        }

    for (i = 0; ConfigureNamesInt[i].name; i++)
        if (strcasecmp(ConfigureNamesInt[i].name, name)==0)
        {
            if (record)
                *record = (struct config_generic *)&ConfigureNamesInt[i];
            return PGC_INT;
        }

    for (i = 0; ConfigureNamesReal[i].name; i++)
        if (strcasecmp(ConfigureNamesReal[i].name, name)==0)
        {
            if (record)
                *record = (struct config_generic *)&ConfigureNamesReal[i];
            return PGC_REAL;
        }

	for (i = 0; ConfigureNamesString[i].name; i++)
        if (strcasecmp(ConfigureNamesString[i].name, name)==0)
        {
            if (record)
                *record = (struct config_generic *)&ConfigureNamesString[i];
            return PGC_STRING;
        }

    return PGC_NONE;
}



/*
 * Reset all options to their specified default values. Should only be
 * called at program startup.
 */
void
ResetAllOptions(void)
{
    int i;

    for (i = 0; ConfigureNamesBool[i].name; i++)
        *(ConfigureNamesBool[i].variable) = ConfigureNamesBool[i].default_val;

    for (i = 0; ConfigureNamesInt[i].name; i++)
        *(ConfigureNamesInt[i].variable) = ConfigureNamesInt[i].default_val;

    for (i = 0; ConfigureNamesReal[i].name; i++)
        *(ConfigureNamesReal[i].variable) = ConfigureNamesReal[i].default_val;

	for (i = 0; ConfigureNamesString[i].name; i++)
	{
		char * str = NULL;

		if (ConfigureNamesString[i].default_val)
		{
			str = strdup(ConfigureNamesString[i].default_val);
			if (str == NULL)
				elog(ERROR, "out of memory");
		}
		*(ConfigureNamesString[i].variable) = str;
	}

	if (getenv("PGPORT"))
		PostPortNumber = atoi(getenv("PGPORT"));
}



/*
 * Try to interpret value as boolean value.  Valid values are: true,
 * false, yes, no, on, off, 1, 0.  If the string parses okay, return
 * true, else false.  If result is not NULL, return the parsing result
 * there.
 */
static bool
parse_bool(const char * value, bool * result)
{
    size_t len = strlen(value);

    if (strncasecmp(value, "true", len)==0)
    {
        if (result)
            *result = true;
    }
    else if (strncasecmp(value, "false", len)==0)
    {
        if (result)
            *result = false;
    }

    else if (strncasecmp(value, "yes", len)==0)
    {
        if (result)
            *result = true;
    }
    else if (strncasecmp(value, "no", len)==0)
    {
        if (result)
            *result = false;
    }

    else if (strcasecmp(value, "on")==0)
    {
        if (result)
            *result = true;
    }
    else if (strcasecmp(value, "off")==0)
    {
        if (result)
            *result = false;
    }

    else if (strcasecmp(value, "1")==0)
    {
        if (result)
            *result = true;
    }
    else if (strcasecmp(value, "0")==0)
    {
        if (result)
            *result = false;
    }

    else
        return false;
    return true;
}



/*
 * Try to parse value as an integer.  The accepted formats are the
 * usual decimal, octal, or hexadecimal formats.  If the string parses
 * okay, return true, else false.  If result is not NULL, return the
 * value there.
 */
static bool
parse_int(const char * value, int * result)
{
    long val;
    char * endptr;

    errno = 0;
    val = strtol(value, &endptr, 0);
    if (endptr == value || *endptr != '\0' || errno == ERANGE)
        return false;
    if (result)
        *result = (int)val;
    return true;
}



/*
 * Try to parse value as a floating point constant in the usual
 * format.  If the value parsed okay return true, else false.  If
 * result is not NULL, return the semantic value there.
 */
static bool
parse_real(const char * value, double * result)
{
    double val;
    char * endptr;

    errno = 0;
    val = strtod(value, &endptr);
    if (endptr == value || *endptr != '\0' || errno == ERANGE)
        return false;
    if (result)
        *result = val;
    return true;
}



/*
 * Sets option `name' to given value. The value should be a string
 * which is going to be parsed and converted to the appropriate data
 * type. Parameter context should indicate in which context this
 * function is being called so it can apply the access restrictions
 * properly.
 *
 * If value is NULL, set the option to its default value. If the
 * parameter DoIt is false then don't really set the option but do all
 * the checks to see if it would work.
 *
 * If there is an error (non-existing option, invalid value) then an
 * elog(ERROR) is thrown *unless* this is called as part of the
 * configuration file re-read in the SIGHUP handler, in which case we
 * simply write the error message via elog(DEBUG) and return false. In
 * all other cases the function returns true. This is working around
 * the deficiencies in the elog mechanism, so don't blame me.
 *
 * See also SetConfigOption for an external interface.
 */
bool
set_config_option(const char * name, const char * value, GucContext
				  context, bool DoIt)
{
    struct config_generic * record;
    enum config_type type;
	int elevel;

	elevel = (context == PGC_SIGHUP) ? DEBUG : ERROR;

    type = find_option(name, &record);
    if (type == PGC_NONE)
	{
		elog(elevel, "'%s' is not a valid option name", name);
		return false;
	}

	/*
	 * Check if the option can be set at this time. See guc.h for the
	 * precise rules. Note that we don't want to throw errors if we're
	 * in the SIGHUP context. In that case we just ignore the attempt.
	 */
    if (record->context == PGC_POSTMASTER && context != PGC_POSTMASTER)
	{
		if (context != PGC_SIGHUP)
			elog(ERROR, "'%s' cannot be changed after server start", name);
		else
			return true;
	}
	else if (record->context == PGC_SIGHUP && context != PGC_SIGHUP &&
			 context != PGC_POSTMASTER)
	{
		elog(ERROR, "'%s' cannot be changed now", name);
		/* Hmm, the idea of the SIGHUP context is "ought to be global,
		 * but can be changed after postmaster start". But there's
		 * nothing that prevents a crafty administrator from sending
		 * SIGHUP signals to individual backends only. */
	}
	else if (record->context == PGC_BACKEND && context != PGC_BACKEND
			 && context != PGC_POSTMASTER)
	{
		if (context != PGC_SIGHUP)
			elog(ERROR, "'%s' cannot be set after connection start", name);
		else
			return true;
	}
	else if (record->context == PGC_SUSET && (context == PGC_USERSET
											  || context == PGC_BACKEND))
	{
		elog(ERROR, "permission denied");
	}


	/*
	 * Evaluate value and set variable
	 */
    switch(type)
    {
        case PGC_BOOL:
		{
			struct config_bool * conf = (struct config_bool *)record;

            if (value)
            {
				bool boolval;
                if (!parse_bool(value, &boolval))
				{
					elog(elevel, "option '%s' requires a boolean value", name);
					return false;
				}
				if (DoIt)
					*conf->variable = boolval;
            }
            else if (DoIt)
                *conf->variable = conf->default_val;
            break;
		}

		case PGC_INT:
        {
            struct config_int * conf = (struct config_int *)record;

            if (value)
            {
                int intval;

                if (!parse_int(value, &intval))
				{
                    elog(elevel, "option '%s' expects an integer value", name);
					return false;
				}
                if (intval < conf->min || intval > conf->max)
				{
                    elog(elevel, "option '%s' value %d is outside"
						 " of permissible range [%d .. %d]",
						 name, intval, conf->min, conf->max);
					return false;
				}
				if (DoIt)
					*conf->variable = intval;
            }
            else if (DoIt)
                *conf->variable = conf->default_val;
            break;
        }

		case PGC_REAL:
        {
            struct config_real * conf = (struct config_real *)record;

            if (value)
            {
                double dval;

                if (!parse_real(value, &dval))
				{
                    elog(elevel, "option '%s' expects a real number", name);
					return false;
				}
                if (dval < conf->min || dval > conf->max)
				{
                    elog(elevel, "option '%s' value %g is outside"
						 " of permissible range [%g .. %g]",
						 name, dval, conf->min, conf->max);
					return false;
				}
				if (DoIt)
					*conf->variable = dval;
            }
            else if (DoIt)
                *conf->variable = conf->default_val;
            break;
        }

		case PGC_STRING:
		{
			struct config_string * conf = (struct config_string *)record;

			if (value)
			{
				if (conf->parse_hook && !(conf->parse_hook)(value))
				{
					elog(elevel, "invalid value for option '%s': '%s'", name, value);
					return false;
				}
				if (DoIt)
				{
					char * str;

					str = strdup(value);
					if (str == NULL)
					{
						elog(elevel, "out of memory");
						return false;
					}
					free(*conf->variable);
					*conf->variable = str;
				}
			}
			else if (DoIt)
			{
				char * str;

				str = strdup(conf->default_val);
				if (str == NULL)
				{
					elog(elevel, "out of memory");
					return false;
				}
				free(*conf->variable);
				*conf->variable = str;
			}
			break;
		}

		default: ;
    }
	return true;
}



/*
 * Set a config option to the given value. See also set_config_option,
 * this is just the wrapper to be called from the outside.
 */
void
SetConfigOption(const char * name, const char * value, GucContext
				context)
{
	(void)set_config_option(name, value, context, true);
}



/*
 * This is more or less the SHOW command. It returns a string with the
 * value of the option `name'. If the option doesn't exist, throw an
 * elog and don't return. issuper should be true if and only if the
 * current user is a superuser. Normal users don't have read
 * permission on all options.
 *
 * The string is *not* allocated for modification and is really only
 * valid until the next call to configuration related functions.
 */
const char *
GetConfigOption(const char * name)
{
    struct config_generic * record;
	static char buffer[256];
	enum config_type opttype;

    opttype = find_option(name, &record);
	if (opttype == PGC_NONE)
		elog(ERROR, "Option '%s' is not recognized", name);

	switch(opttype)
    {
        case PGC_BOOL:
            return *((struct config_bool *)record)->variable ? "on" : "off";

        case PGC_INT:
			snprintf(buffer, 256, "%d", *((struct config_int *)record)->variable);
			return buffer;

        case PGC_REAL:
			snprintf(buffer, 256, "%g", *((struct config_real *)record)->variable);
			return buffer;

		case PGC_STRING:
			return *((struct config_string *)record)->variable;

        default:
			;
    }
    return NULL;
}    



/*
 * A little "long argument" simulation, although not quite GNU
 * compliant. Takes a string of the form "some-option=some value" and
 * returns name = "some_option" and value = "some value" in malloc'ed
 * storage. Note that '-' is converted to '_' in the option name. If
 * there is no '=' in the input string then value will be NULL.
 */
void
ParseLongOption(const char * string, char ** name, char ** value)
{
	size_t equal_pos;
	char *cp;

	AssertArg(string);
	AssertArg(name);
	AssertArg(value);

	equal_pos = strcspn(string, "=");

	if (string[equal_pos] == '=')
	{
		*name = malloc(equal_pos + 1);
		if (!*name)
			elog(FATAL, "out of memory");
		strncpy(*name, string, equal_pos);
		(*name)[equal_pos] = '\0';

		*value = strdup(&string[equal_pos + 1]);
		if (!*value)
			elog(FATAL, "out of memory");
	}
	else						/* no equal sign in string */
	{
		*name = strdup(string);
		if (!*name)
			elog(FATAL, "out of memory");
		*value = NULL;
	}

	for(cp = *name; *cp; cp++)
		if (*cp == '-')
			*cp = '_';
}



#ifdef ENABLE_SYSLOG
bool 
check_facility(const char *facility)
{
	if (strcasecmp(facility,"LOCAL0") == 0) return true;
	if (strcasecmp(facility,"LOCAL1") == 0) return true;
	if (strcasecmp(facility,"LOCAL2") == 0) return true;
	if (strcasecmp(facility,"LOCAL3") == 0) return true;
	if (strcasecmp(facility,"LOCAL4") == 0) return true;
	if (strcasecmp(facility,"LOCAL5") == 0) return true;
	if (strcasecmp(facility,"LOCAL6") == 0) return true;
	if (strcasecmp(facility,"LOCAL7") == 0) return true;
	return false;
}
#endif
