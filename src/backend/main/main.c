/*-------------------------------------------------------------------------
 *
 * main.c
 *	  Stub main() routine for the postgres executable.
 *
 * This does some essential startup tasks for any incarnation of postgres
 * (postmaster, standalone backend, standalone bootstrap process, or a
 * separately exec'd child of a postmaster) and then dispatches to the
 * proper FooMain() routine for the incarnation.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/main/main.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "nodes/print.h"
#include "commands/explain.h"

#if defined(WIN32)
#include <crtdbg.h>
#endif

#if defined(__NetBSD__)
#include <sys/param.h>
#endif

#include "bootstrap/bootstrap.h"
#include "common/username.h"
#include "port/atomics.h"
#include "postmaster/postmaster.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/help_config.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/ps_status.h"
#include "access/xact.h"
#include "parser/analyze.h"
#include "catalog/pg_type_d.h"
#include "pgplanner/pgplanner.h"

const char *progname;
static bool reached_main = false;


static void startup_hacks(const char *progname);
static void init_locale(const char *categoryname, int category, const char *locale);
static void help(const char *progname);
static void check_root(const char *progname);


/* ----------------------------------------------------------------
 *	Sample callbacks for demonstration
 * ----------------------------------------------------------------
 */

static PgPlannerColumn my_table_columns[] = {
	{ "a", INT4OID, -1 }
};

static PgPlannerRelationInfo my_table_info = {
	.relid = 1337,
	.relname = "my_table",
	.relkind = 'r',		/* RELKIND_RELATION */
	.natts = 1,
	.columns = my_table_columns
};

static PgPlannerRelationInfo *
sample_get_relation(const char *schemaname, const char *relname)
{
	if (strcmp(relname, "my_table") == 0)
		return &my_table_info;
	return NULL;
}

static PgPlannerRelationInfo *
sample_get_relation_by_oid(Oid relid)
{
	if (relid == 1337)
		return &my_table_info;
	return NULL;
}

static PgPlannerOperatorInfo eq_op_info = {
	.oprid = 96,		/* int4eq OID in standard PG */
	.oprcode = 65,		/* int4eq function OID */
	.oprleft = 23,		/* INT4OID */
	.oprright = 23,
	.oprresult = 16		/* BOOLOID */
};

static PgPlannerOperatorInfo *
sample_get_operator(const char *opname, Oid left_type, Oid right_type)
{
	if (left_type == INT4OID && right_type == INT4OID)
		return &eq_op_info;
	return NULL;
}

static PgPlannerTypeInfo int4_type_info = {
	.typlen = 4, .typbyval = true, .typalign = 'i', .typtype = 'b',
	.typbasetype = 0, .typtypmod = -1,
	.typname = "int4", .typnamespace = 11, .typowner = 10,
	.typcategory = 'N', .typispreferred = false, .typisdefined = true,
	.typdelim = ',', .typrelid = 0, .typsubscript = 0,
	.typelem = 0, .typarray = 1007,
	.typinput = 42, .typoutput = 43, .typreceive = 2406, .typsend = 2407,
	.typmodin = 0, .typmodout = 0, .typanalyze = 0,
	.typstorage = 'p', .typnotnull = false, .typndims = 0, .typcollation = 0
};

static PgPlannerTypeInfo bool_type_info = {
	.typlen = 1, .typbyval = true, .typalign = 'c', .typtype = 'b',
	.typbasetype = 0, .typtypmod = -1,
	.typname = "bool", .typnamespace = 11, .typowner = 10,
	.typcategory = 'B', .typispreferred = true, .typisdefined = true,
	.typdelim = ',', .typrelid = 0, .typsubscript = 0,
	.typelem = 0, .typarray = 1000,
	.typinput = 1242, .typoutput = 1243, .typreceive = 2436, .typsend = 2437,
	.typmodin = 0, .typmodout = 0, .typanalyze = 0,
	.typstorage = 'p', .typnotnull = false, .typndims = 0, .typcollation = 0
};

static PgPlannerTypeInfo int8_type_info = {
	.typlen = 8, .typbyval = true, .typalign = 'd', .typtype = 'b',
	.typbasetype = 0, .typtypmod = -1,
	.typname = "int8", .typnamespace = 11, .typowner = 10,
	.typcategory = 'N', .typispreferred = false, .typisdefined = true,
	.typdelim = ',', .typrelid = 0, .typsubscript = 0,
	.typelem = 0, .typarray = 1016,
	.typinput = 461, .typoutput = 462, .typreceive = 2408, .typsend = 2409,
	.typmodin = 0, .typmodout = 0, .typanalyze = 0,
	.typstorage = 'p', .typnotnull = false, .typndims = 0, .typcollation = 0
};

static PgPlannerTypeInfo *
sample_get_type(Oid typid)
{
	if (typid == INT4OID)
		return &int4_type_info;
	if (typid == BOOLOID)
		return &bool_type_info;
	if (typid == INT8OID)
		return &int8_type_info;
	return NULL;
}

/* count(*) function info (OID 2803) */
static PgPlannerFunctionInfo count_func_info = {
	.retset = false,
	.rettype = 20,		/* INT8OID */
	.prokind = 'a',		/* aggregate */
	.proisstrict = false,
	.pronargs = 0,
	.proargtypes = NULL,
	.provariadic = 0,	/* InvalidOid */
	.proname = "count",
	.pronamespace = 11,	/* PG_CATALOG_NAMESPACE */
	.provolatile = 'i',
	.proparallel = 's'
};

/* int8inc transition function (OID 2804) */
static PgPlannerFunctionInfo int8inc_func_info = {
	.retset = false,
	.rettype = 20,		/* INT8OID */
	.prokind = 'f',
	.proisstrict = true,
	.pronargs = 1,
	.proargtypes = (Oid[]){20},	/* INT8OID */
	.provariadic = 0,
	.proname = "int8inc",
	.pronamespace = 11,
	.provolatile = 'i',
	.proparallel = 's'
};

/* int8pl combine function (OID 1279) */
static PgPlannerFunctionInfo int8pl_func_info = {
	.retset = false,
	.rettype = 20,		/* INT8OID */
	.prokind = 'f',
	.proisstrict = true,
	.pronargs = 2,
	.proargtypes = (Oid[]){20, 20},
	.provariadic = 0,
	.proname = "int8pl",
	.pronamespace = 11,
	.provolatile = 'i',
	.proparallel = 's'
};

static PgPlannerFunctionInfo int4eq_func_info = {
	.retset = false,
	.rettype = 16,		/* BOOLOID */
	.prokind = 'f',
	.proisstrict = true,
	.pronargs = 2,
	.proargtypes = (Oid[]){23, 23},
	.provariadic = 0,
	.proname = "int4eq",
	.pronamespace = 11,
	.provolatile = 'i',
	.proparallel = 's'
};

static PgPlannerFunctionInfo *
sample_get_function(Oid funcid)
{
	if (funcid == 2803)
		return &count_func_info;
	if (funcid == 2804)
		return &int8inc_func_info;
	if (funcid == 1279)
		return &int8pl_func_info;
	if (funcid == 65)
		return &int4eq_func_info;
	return NULL;
}

/* count(*) function candidate */
static PgPlannerFuncCandidate count_candidate = {
	.oid = 2803,
	.nargs = 0,
	.argtypes = NULL,
	.variadic_type = 0,		/* InvalidOid */
	.ndargs = 0
};

static int
sample_get_func_candidates(const char *funcname,
						   PgPlannerFuncCandidate **candidates_out)
{
	if (strcmp(funcname, "count") == 0)
	{
		*candidates_out = &count_candidate;
		return 1;
	}
	*candidates_out = NULL;
	return 0;
}

/* count aggregate info */
static PgPlannerAggregateInfo count_agg_info = {
	.aggkind = 'n',
	.aggnumdirectargs = 0,
	.aggtransfn = 2804,		/* int8inc */
	.aggfinalfn = 0,		/* InvalidOid */
	.aggcombinefn = 1279,	/* int8pl */
	.aggserialfn = 0,
	.aggdeserialfn = 0,
	.aggtranstype = 20,		/* INT8OID */
	.aggtransspace = 0,
	.aggfinalmodify = 'r',	/* read-only */
	.aggsortop = 0,			/* InvalidOid */
	.agginitval = "0"
};

static PgPlannerAggregateInfo *
sample_get_aggregate(Oid aggfnoid)
{
	if (aggfnoid == 2803)
		return &count_agg_info;
	return NULL;
}

/*
 * Demo main: plan a simple query using the callback-based API.
 */
int
main(int argc, char *argv[])
{
	PlannedStmt *stmt;
	char	   *str;

	PgPlannerCallbacks callbacks = {
		.get_relation = sample_get_relation,
		.get_relation_by_oid = sample_get_relation_by_oid,
		.get_operator = sample_get_operator,
		.get_type = sample_get_type,
		.get_function = sample_get_function,
		.get_func_candidates = sample_get_func_candidates,
		.get_aggregate = sample_get_aggregate
	};

	pgplanner_init();

	printf("pgplanner demo\n");

	stmt = pgplanner_plan_query("SELECT COUNT(*) FROM my_table;", &callbacks);

	str = nodeToString(stmt);
	printf("PlannedStmt: %s\n", str);
	pfree(str);

	return 0;
}



/*
 * Place platform-specific startup hacks here.  This is the right
 * place to put code that must be executed early in the launch of any new
 * server process.  Note that this code will NOT be executed when a backend
 * or sub-bootstrap process is forked, unless we are in a fork/exec
 * environment (ie EXEC_BACKEND is defined).
 *
 * XXX The need for code here is proof that the platform in question
 * is too brain-dead to provide a standard C execution environment
 * without help.  Avoid adding more here, if you can.
 */
static void
startup_hacks(const char *progname)
{
	/*
	 * Windows-specific execution environment hacking.
	 */
#ifdef WIN32
	{
		WSADATA		wsaData;
		int			err;

		/* Make output streams unbuffered by default */
		setvbuf(stdout, NULL, _IONBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);

		/* Prepare Winsock */
		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err != 0)
		{
			write_stderr("%s: WSAStartup failed: %d\n",
						 progname, err);
			exit(1);
		}

		/*
		 * By default abort() only generates a crash-dump in *non* debug
		 * builds. As our Assert() / ExceptionalCondition() uses abort(),
		 * leaving the default in place would make debugging harder.
		 *
		 * MINGW's own C runtime doesn't have _set_abort_behavior(). When
		 * targeting Microsoft's UCRT with mingw, it never links to the debug
		 * version of the library and thus doesn't need the call to
		 * _set_abort_behavior() either.
		 */
#if !defined(__MINGW32__) && !defined(__MINGW64__)
		_set_abort_behavior(_CALL_REPORTFAULT | _WRITE_ABORT_MSG,
							_CALL_REPORTFAULT | _WRITE_ABORT_MSG);
#endif							/* !defined(__MINGW32__) &&
								 * !defined(__MINGW64__) */

		/*
		 * SEM_FAILCRITICALERRORS causes more errors to be reported to
		 * callers.
		 *
		 * We used to also specify SEM_NOGPFAULTERRORBOX, but that prevents
		 * windows crash reporting from working. Which includes registered
		 * just-in-time debuggers, making it unnecessarily hard to debug
		 * problems on windows. Now we try to disable sources of popups
		 * separately below (note that SEM_NOGPFAULTERRORBOX did not actually
		 * prevent all sources of such popups).
		 */
		SetErrorMode(SEM_FAILCRITICALERRORS);

		/*
		 * Show errors on stderr instead of popup box (note this doesn't
		 * affect errors originating in the C runtime, see below).
		 */
		_set_error_mode(_OUT_TO_STDERR);

		/*
		 * In DEBUG builds, errors, including assertions, C runtime errors are
		 * reported via _CrtDbgReport. By default such errors are displayed
		 * with a popup (even with NOGPFAULTERRORBOX), preventing forward
		 * progress. Instead report such errors stderr (and the debugger).
		 * This is C runtime specific and thus the above incantations aren't
		 * sufficient to suppress these popups.
		 */
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
	}
#endif							/* WIN32 */

	/*
	 * Initialize dummy_spinlock, in case we are on a platform where we have
	 * to use the fallback implementation of pg_memory_barrier().
	 */
	SpinLockInit(&dummy_spinlock);
}


/*
 * Make the initial permanent setting for a locale category.  If that fails,
 * perhaps due to LC_foo=invalid in the environment, use locale C.  If even
 * that fails, perhaps due to out-of-memory, the entire startup fails with it.
 * When this returns, we are guaranteed to have a setting for the given
 * category's environment variable.
 */
static void
init_locale(const char *categoryname, int category, const char *locale)
{
	if (pg_perm_setlocale(category, locale) == NULL &&
		pg_perm_setlocale(category, "C") == NULL)
		elog(FATAL, "could not adopt \"%s\" locale nor C locale for %s",
			 locale, categoryname);
}



/*
 * Help display should match the options accepted by PostmasterMain()
 * and PostgresMain().
 *
 * XXX On Windows, non-ASCII localizations of these messages only display
 * correctly if the console output code page covers the necessary characters.
 * Messages emitted in write_console() do not exhibit this problem.
 */
static void
help(const char *progname)
{
	printf(_("%s is the PostgreSQL server.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -B NBUFFERS        number of shared buffers\n"));
	printf(_("  -c NAME=VALUE      set run-time parameter\n"));
	printf(_("  -C NAME            print value of run-time parameter, then exit\n"));
	printf(_("  -d 1-5             debugging level\n"));
	printf(_("  -D DATADIR         database directory\n"));
	printf(_("  -e                 use European date input format (DMY)\n"));
	printf(_("  -F                 turn fsync off\n"));
	printf(_("  -h HOSTNAME        host name or IP address to listen on\n"));
	printf(_("  -i                 enable TCP/IP connections (deprecated)\n"));
	printf(_("  -k DIRECTORY       Unix-domain socket location\n"));
#ifdef USE_SSL
	printf(_("  -l                 enable SSL connections\n"));
#endif
	printf(_("  -N MAX-CONNECT     maximum number of allowed connections\n"));
	printf(_("  -p PORT            port number to listen on\n"));
	printf(_("  -s                 show statistics after each query\n"));
	printf(_("  -S WORK-MEM        set amount of memory for sorts (in kB)\n"));
	printf(_("  -V, --version      output version information, then exit\n"));
	printf(_("  --NAME=VALUE       set run-time parameter\n"));
	printf(_("  --describe-config  describe configuration parameters, then exit\n"));
	printf(_("  -?, --help         show this help, then exit\n"));

	printf(_("\nDeveloper options:\n"));
	printf(_("  -f s|i|o|b|t|n|m|h forbid use of some plan types\n"));
	printf(_("  -O                 allow system table structure changes\n"));
	printf(_("  -P                 disable system indexes\n"));
	printf(_("  -t pa|pl|ex        show timings after each query\n"));
	printf(_("  -T                 send SIGABRT to all backend processes if one dies\n"));
	printf(_("  -W NUM             wait NUM seconds to allow attach from a debugger\n"));

	printf(_("\nOptions for single-user mode:\n"));
	printf(_("  --single           selects single-user mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (defaults to user name)\n"));
	printf(_("  -d 0-5             override debugging level\n"));
	printf(_("  -E                 echo statement before execution\n"));
	printf(_("  -j                 do not use newline as interactive query delimiter\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nOptions for bootstrapping mode:\n"));
	printf(_("  --boot             selects bootstrapping mode (must be first argument)\n"));
	printf(_("  --check            selects check mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (mandatory argument in bootstrapping mode)\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nPlease read the documentation for the complete list of run-time\n"
			 "configuration settings and how to set them on the command line or in\n"
			 "the configuration file.\n\n"
			 "Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}



static void
check_root(const char *progname)
{
#ifndef WIN32
	if (geteuid() == 0)
	{
		write_stderr("\"root\" execution of the PostgreSQL server is not permitted.\n"
					 "The server must be started under an unprivileged user ID to prevent\n"
					 "possible system security compromise.  See the documentation for\n"
					 "more information on how to properly start the server.\n");
		exit(1);
	}

	/*
	 * Also make sure that real and effective uids are the same. Executing as
	 * a setuid program from a root shell is a security hole, since on many
	 * platforms a nefarious subroutine could setuid back to root if real uid
	 * is root.  (Since nobody actually uses postgres as a setuid program,
	 * trying to actively fix this situation seems more trouble than it's
	 * worth; we'll just expend the effort to check for it.)
	 */
	if (getuid() != geteuid())
	{
		write_stderr("%s: real and effective user IDs must match\n",
					 progname);
		exit(1);
	}
#else							/* WIN32 */
	if (pgwin32_is_admin())
	{
		write_stderr("Execution of PostgreSQL by a user with administrative permissions is not\n"
					 "permitted.\n"
					 "The server must be started under an unprivileged user ID to prevent\n"
					 "possible system security compromises.  See the documentation for\n"
					 "more information on how to properly start the server.\n");
		exit(1);
	}
#endif							/* WIN32 */
}

/*
 * At least on linux, set_ps_display() breaks /proc/$pid/environ. The
 * sanitizer library uses /proc/$pid/environ to implement getenv() as it wants
 * to work independent of libc. When just using undefined and alignment
 * sanitizers, the sanitizer library is only initialized when the first error
 * occurs, by which time we've often already called set_ps_display(),
 * preventing the sanitizer libraries from seeing the options.
 *
 * We can work around that by defining __ubsan_default_options, a weak symbol
 * libsanitizer uses to get defaults from the application, and return
 * getenv("UBSAN_OPTIONS"). But only if main already was reached, so that we
 * don't end up relying on a not-yet-working getenv().
 *
 * As this function won't get called when not running a sanitizer, it doesn't
 * seem necessary to only compile it conditionally.
 */
const char *__ubsan_default_options(void);
const char *
__ubsan_default_options(void)
{
	/* don't call libc before it's guaranteed to be initialized */
	if (!reached_main)
		return "";

	return getenv("UBSAN_OPTIONS");
}