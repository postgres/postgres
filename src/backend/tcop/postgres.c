/*-------------------------------------------------------------------------
 *
 * postgres.c
 *	  POSTGRES C Backend Interface
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/postgres.c,v 1.152 2000/04/23 00:13:16 tgl Exp $
 *
 * NOTES
 *	  this is the "main" module of the postgres backend and
 *	  hence the main module of the "traffic cop".
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "postgres.h"

#include <errno.h>
#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif	 /* aix */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "commands/async.h"
#include "commands/trigger.h"
#include "commands/variable.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/print.h"
#include "optimizer/cost.h"
#include "optimizer/planner.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/fastpath.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "storage/proc.h"
#include "utils/ps_status.h"
#include "utils/temprel.h"
#include "utils/trace.h"

#include "../backend/parser/parse.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

/*
 * Trace flags, see backend/utils/misc/trace.c
 */
#define Verbose				pg_options[TRACE_VERBOSE]
#define DebugPrintQuery		pg_options[TRACE_QUERY]
#define DebugPrintPlan		pg_options[TRACE_PLAN]
#define DebugPrintParse		pg_options[TRACE_PARSE]
#define DebugPrintRewrittenParsetree \
							pg_options[TRACE_REWRITTEN]
#define DebugPPrintPlan		pg_options[TRACE_PRETTY_PLAN]
#define DebugPPrintParse	pg_options[TRACE_PRETTY_PARSE]
#define DebugPPrintRewrittenParsetree \
							pg_options[TRACE_PRETTY_REWRITTEN]
#define ShowParserStats		pg_options[TRACE_PARSERSTATS]
#define ShowPlannerStats	pg_options[TRACE_PLANNERSTATS]
#define ShowExecutorStats	pg_options[TRACE_EXECUTORSTATS]
#ifdef LOCK_MGR_DEBUG
#define LockDebug			pg_options[TRACE_LOCKS]
#endif

#define DeadlockCheckTimer	pg_options[OPT_DEADLOCKTIMEOUT]
#define HostnameLookup		pg_options[OPT_HOSTLOOKUP]
#define ShowPortNumber		pg_options[OPT_SHOWPORTNUMBER]

/* ----------------
 *		global variables
 * ----------------
 */

CommandDest whereToSendOutput = Debug;

/* Define status buffer needed by PS_SET_STATUS */
#ifdef PS_DEFINE_BUFFER
PS_DEFINE_BUFFER;
#endif

extern void BaseInit(void);
extern void StartupXLOG(void);
extern void ShutdownXLOG(void);

extern void HandleDeadLock(SIGNAL_ARGS);

extern char XLogDir[];
extern char ControlFilePath[];

extern int	lockingOff;
extern int	NBuffers;

int			dontExecute = 0;
static int	ShowStats;
static bool IsEmptyQuery = false;

/* note: these declarations had better match tcopprot.h */
DLLIMPORT sigjmp_buf Warn_restart;

bool		Warn_restart_ready = false;
bool		InError = false;
bool		ExitAfterAbort = false;

extern int	NBuffers;

static bool EchoQuery = false;	/* default don't echo */
time_t		tim;
char		pg_pathname[MAXPGPATH];
FILE	   *StatFp;

/* ----------------
 *		people who want to use EOF should #define DONTUSENEWLINE in
 *		tcop/tcopdebug.h
 * ----------------
 */
#ifndef TCOP_DONTUSENEWLINE
int			UseNewLine = 1;		/* Use newlines query delimiters (the
								 * default) */

#else
int			UseNewLine = 0;		/* Use EOF as query delimiters */

#endif	 /* TCOP_DONTUSENEWLINE */

/*
** Flags for expensive function optimization -- JMH 3/9/92
*/
int			XfuncMode = 0;

/* ----------------------------------------------------------------
 *		decls for routines only used in this file
 * ----------------------------------------------------------------
 */
static int	InteractiveBackend(StringInfo inBuf);
static int	SocketBackend(StringInfo inBuf);
static int	ReadCommand(StringInfo inBuf);
static void pg_exec_query(char *query_string);


/* ----------------------------------------------------------------
 *		routines to obtain user input
 * ----------------------------------------------------------------
 */

/* ----------------
 *	InteractiveBackend() is called for user interactive connections
 *	the string entered by the user is placed in its parameter inBuf.
 *
 *	EOF is returned if end-of-file input is seen; time to shut down.
 * ----------------
 */

static int
InteractiveBackend(StringInfo inBuf)
{
	int			c;				/* character read from getc() */
	bool		end = false;	/* end-of-input flag */
	bool		backslashSeen = false;	/* have we seen a \ ? */

	/* ----------------
	 *	display a prompt and obtain input from the user
	 * ----------------
	 */
	printf("backend> ");
	fflush(stdout);

	/* Reset inBuf to empty */
	inBuf->len = 0;
	inBuf->data[0] = '\0';

	for (;;)
	{
		if (UseNewLine)
		{
			/* ----------------
			 *	if we are using \n as a delimiter, then read
			 *	characters until the \n.
			 * ----------------
			 */
			while ((c = getc(stdin)) != EOF)
			{
				if (c == '\n')
				{
					if (backslashSeen)
					{
						/* discard backslash from inBuf */
						inBuf->data[--inBuf->len] = '\0';
						backslashSeen = false;
						continue;
					}
					else
					{
						/* keep the newline character */
						appendStringInfoChar(inBuf, '\n');
						break;
					}
				}
				else if (c == '\\')
					backslashSeen = true;
				else
					backslashSeen = false;

				appendStringInfoChar(inBuf, (char) c);
			}

			if (c == EOF)
				end = true;
		}
		else
		{
			/* ----------------
			 *	otherwise read characters until EOF.
			 * ----------------
			 */
			while ((c = getc(stdin)) != EOF)
				appendStringInfoChar(inBuf, (char) c);

			if (inBuf->len == 0)
				end = true;
		}

		if (end)
		{
			if (Verbose)
				puts("EOF");
			return EOF;
		}

		/* ----------------
		 *	otherwise we have a user query so process it.
		 * ----------------
		 */
		break;
	}

	/* ----------------
	 *	if the query echo flag was given, print the query..
	 * ----------------
	 */
	if (EchoQuery)
		printf("query: %s\n", inBuf->data);
	fflush(stdout);

	return 'Q';
}

/* ----------------
 *	SocketBackend()		Is called for frontend-backend connections
 *
 *	If the input is a query (case 'Q') then the string entered by
 *	the user is placed in its parameter inBuf.
 *
 *	If the input is a fastpath function call (case 'F') then
 *	the function call is processed in HandleFunctionRequest()
 *	(now called from PostgresMain()).
 *
 *	EOF is returned if the connection is lost.
 * ----------------
 */

static int
SocketBackend(StringInfo inBuf)
{
	char		qtype;
	char		result = '\0';

	/* ----------------
	 *	get input from the frontend
	 * ----------------
	 */
	qtype = '?';
	if (pq_getbytes(&qtype, 1) == EOF)
		return EOF;

	switch (qtype)
	{
			/* ----------------
			 *	'Q': user entered a query
			 * ----------------
			 */
		case 'Q':
			if (pq_getstr(inBuf))
				return EOF;
			result = 'Q';
			break;

			/* ----------------
			 *	'F':  calling user/system functions
			 * ----------------
			 */
		case 'F':
			if (pq_getstr(inBuf))
				return EOF;		/* ignore "string" at start of F message */
			result = 'F';
			break;

			/* ----------------
			 *	'X':  frontend is exiting
			 * ----------------
			 */
		case 'X':
			result = 'X';
			break;

			/* ----------------
			 *	otherwise we got garbage from the frontend.
			 *
			 *	XXX are we certain that we want to do an elog(FATAL) here?
			 *		-cim 1/24/90
			 * ----------------
			 */
		default:
			elog(FATAL, "Socket command type %c unknown", qtype);
			break;
	}
	return result;
}

/* ----------------
 *		ReadCommand reads a command from either the frontend or
 *		standard input, places it in inBuf, and returns a char
 *		representing whether the string is a 'Q'uery or a 'F'astpath
 *		call.  EOF is returned if end of file.
 * ----------------
 */
static int
ReadCommand(StringInfo inBuf)
{
	MemoryContext oldcontext;
	int			result;

	/*
	 * Make sure any expansion of inBuf happens in permanent memory
	 * context, so that we can keep using it for future command cycles.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	if (IsUnderPostmaster)
		result = SocketBackend(inBuf);
	else
		result = InteractiveBackend(inBuf);
	MemoryContextSwitchTo(oldcontext);
	return result;
}


/*
 * Parse a query string and pass it through the rewriter.
 *
 * A list of Query nodes is returned, since the string might contain
 * multiple queries and/or the rewriter might expand one query to several.
 */
List *
pg_parse_and_rewrite(char *query_string,		/* string to execute */
					 Oid *typev,/* argument types */
					 int nargs, /* number of arguments */
					 bool aclOverride)
{
	List	   *querytree_list;
	List	   *querytree_list_item;
	Query	   *querytree;
	List	   *new_list;

	if (DebugPrintQuery)
		TPRINTF(TRACE_QUERY, "query: %s", query_string);

	/* ----------------
	 *	(1) parse the request string into a list of parse trees
	 * ----------------
	 */
	if (ShowParserStats)
		ResetUsage();

	querytree_list = parser(query_string, typev, nargs);

	if (ShowParserStats)
	{
		fprintf(stderr, "! Parser Stats:\n");
		ShowUsage();
	}

	/* ----------------
	 *	(2) rewrite the queries, as necessary
	 *
	 *	rewritten queries are collected in new_list.  Note there may be
	 *	more or fewer than in the original list.
	 * ----------------
	 */
	new_list = NIL;
	foreach(querytree_list_item, querytree_list)
	{
		querytree = (Query *) lfirst(querytree_list_item);

		if (DebugPrintParse || DebugPPrintParse)
		{
			if (DebugPPrintParse)
			{
				TPRINTF(TRACE_PRETTY_PARSE, "parser outputs:");
				nodeDisplay(querytree);
			}
			else
			{
				TPRINTF(TRACE_PARSE, "parser outputs:");
				printf("\n%s\n\n", nodeToString(querytree));
			}
		}

		if (querytree->commandType == CMD_UTILITY)
		{
			/* don't rewrite utilities, just dump 'em into new_list */
			new_list = lappend(new_list, querytree);
		}
		else
		{
			/* rewrite regular queries */
			List	   *rewritten = QueryRewrite(querytree);

			new_list = nconc(new_list, rewritten);
		}
	}

	querytree_list = new_list;

	/* ----------------
	 *	(3) If ACL override is requested, mark queries for no ACL check.
	 * ----------------
	 */
	if (aclOverride)
	{
		foreach(querytree_list_item, querytree_list)
		{
			List	   *l;

			querytree = (Query *) lfirst(querytree_list_item);

			if (querytree->commandType == CMD_UTILITY)
				continue;

			foreach(l, querytree->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

				rte->skipAcl = TRUE;
			}
		}
	}

	if (DebugPrintRewrittenParsetree || DebugPPrintRewrittenParsetree)
	{
		if (DebugPPrintRewrittenParsetree)
		{
			TPRINTF(TRACE_PRETTY_REWRITTEN, "after rewriting:");

			foreach(querytree_list_item, querytree_list)
			{
				querytree = (Query *) lfirst(querytree_list_item);
				nodeDisplay(querytree);
				printf("\n");
			}
		}
		else
		{
			TPRINTF(TRACE_REWRITTEN, "after rewriting:");

			foreach(querytree_list_item, querytree_list)
			{
				querytree = (Query *) lfirst(querytree_list_item);
				printf("\n%s\n\n", nodeToString(querytree));
			}
		}
	}

	return querytree_list;
}


/* Generate a plan for a single query. */
Plan *
pg_plan_query(Query *querytree)
{
	Plan	   *plan;

	/* Utility commands have no plans. */
	if (querytree->commandType == CMD_UTILITY)
		return NULL;

	if (ShowPlannerStats)
		ResetUsage();

	/* call that optimizer */
	plan = planner(querytree);

	if (ShowPlannerStats)
	{
		fprintf(stderr, "! Planner Stats:\n");
		ShowUsage();
	}

	/* ----------------
	 *	Print plan if debugging.
	 * ----------------
	 */
	if (DebugPrintPlan || DebugPPrintPlan)
	{
		if (DebugPPrintPlan)
		{
			TPRINTF(TRACE_PRETTY_PLAN, "plan:");
			nodeDisplay(plan);
		}
		else
		{
			TPRINTF(TRACE_PLAN, "plan:");
			printf("\n%s\n\n", nodeToString(plan));
		}
	}

	return plan;
}


/* ----------------------------------------------------------------
 *		pg_exec_query()
 *
 *		Takes a querystring, runs the parser/utilities or
 *		parser/planner/executor over it as necessary
 *		Begin Transaction Should have been called before this
 *		and CommitTransaction After this is called
 *		This is strictly because we do not allow for nested xactions.
 *
 *		NON-OBVIOUS-RESTRICTIONS
 *		this function _MUST_ allocate a new "parsetree" each time,
 *		since it may be stored in a named portal and should not
 *		change its value.
 *
 * ----------------------------------------------------------------
 */

static void
pg_exec_query(char *query_string)
{
	pg_exec_query_dest(query_string, whereToSendOutput, FALSE);
}

void
pg_exec_query_acl_override(char *query_string)
{
	pg_exec_query_dest(query_string, whereToSendOutput, TRUE);
}

void
pg_exec_query_dest(char *query_string,	/* string to execute */
				   CommandDest dest,	/* where results should go */
				   bool aclOverride)	/* to give utility commands power
										 * of superusers */
{
	List	   *querytree_list;

	/* parse and rewrite the queries */
	querytree_list = pg_parse_and_rewrite(query_string, NULL, 0,
										  aclOverride);

	/*
	 * NOTE: we do not use "foreach" here because we want to be sure the
	 * list pointer has been advanced before the query is executed. We
	 * need to do that because VACUUM has a nasty little habit of doing
	 * CommitTransactionCommand at startup, and that will release the
	 * memory holding our parse list :-(.  This needs a better solution
	 * --- currently, the code will crash if someone submits "vacuum;
	 * something-else" in a single query string.  But memory allocation
	 * needs redesigned anyway, so this will have to do for now.
	 */
	while (querytree_list)
	{
		Query	   *querytree = (Query *) lfirst(querytree_list);

		querytree_list = lnext(querytree_list);

		/* if we got a cancel signal in parsing or prior command, quit */
		if (QueryCancel)
			CancelQuery();

		if (querytree->commandType == CMD_UTILITY)
		{
			/* ----------------
			 *	 process utility functions (create, destroy, etc..)
			 *
			 *	 Note: we do not check for the transaction aborted state
			 *	 because that is done in ProcessUtility.
			 * ----------------
			 */
			if (DebugPrintQuery)
				TPRINTF(TRACE_QUERY, "ProcessUtility: %s", query_string);
			else if (Verbose)
				TPRINTF(TRACE_VERBOSE, "ProcessUtility");

			ProcessUtility(querytree->utilityStmt, dest);
		}
		else
		{
			Plan	   *plan;

			/* If aborted transaction, skip planning and execution */
			if (IsAbortedTransactionBlockState())
			{
				/* ----------------
				 *	 the EndCommand() stuff is to tell the frontend
				 *	 that the command ended. -cim 6/1/90
				 * ----------------
				 */
				char	   *tag = "*ABORT STATE*";

				elog(NOTICE, "current transaction is aborted, "
					 "queries ignored until end of transaction block");

				EndCommand(tag, dest);

				/*
				 * We continue in the loop, on the off chance that there
				 * is a COMMIT or ROLLBACK utility command later in the
				 * query string.
				 */
				continue;
			}

			plan = pg_plan_query(querytree);

			/* if we got a cancel signal whilst planning, quit */
			if (QueryCancel)
				CancelQuery();

			/* Initialize snapshot state for query */
			SetQuerySnapshot();

			/*
			 * execute the plan
			 */
			if (ShowExecutorStats)
				ResetUsage();

			if (Verbose)
				TPRINTF(TRACE_VERBOSE, "ProcessQuery");
			ProcessQuery(querytree, plan, dest);

			if (ShowExecutorStats)
			{
				fprintf(stderr, "! Executor Stats:\n");
				ShowUsage();
			}
		}

		/*
		 * In a query block, we want to increment the command counter
		 * between queries so that the effects of early queries are
		 * visible to subsequent ones.
		 */

		CommandCounterIncrement();
	}
}

/* --------------------------------
 *		signal handler routines used in PostgresMain()
 *
 *		handle_warn() catches SIGQUIT.	It forces control back to the main
 *		loop, just as if an internal error (elog(ERROR,...)) had occurred.
 *		elog() used to actually use kill(2) to induce a SIGQUIT to get here!
 *		But that's not 100% reliable on some systems, so now it does its own
 *		siglongjmp() instead.
 *		We still provide the signal catcher so that an error quit can be
 *		forced externally.	This should be done only with great caution,
 *		however, since an asynchronous signal could leave the system in
 *		who-knows-what inconsistent state.
 *
 *		quickdie() occurs when signalled by the postmaster.
 *		Some backend has bought the farm,
 *		so we need to stop what we're doing and exit.
 *
 *		die() performs an orderly cleanup via proc_exit()
 * --------------------------------
 */

void
handle_warn(SIGNAL_ARGS)
{
	siglongjmp(Warn_restart, 1);
}

void
quickdie(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);
	elog(NOTICE, "Message from PostgreSQL backend:"
		 "\n\tThe Postmaster has informed me that some other backend"
		 " died abnormally and possibly corrupted shared memory."
		 "\n\tI have rolled back the current transaction and am"
		 " going to terminate your database system connection and exit."
	"\n\tPlease reconnect to the database system and repeat your query.");


	/*
	 * DO NOT proc_exit(0) -- we're here because shared memory may be
	 * corrupted, so we don't want to flush any shared state to stable
	 * storage.  Just nail the windows shut and get out of town.
	 */

	exit(1);
}

/*
 * Abort transaction and exit
 */
void
die(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);

	/*
	 * If ERROR/FATAL is in progress...
	 */
	if (InError)
	{
		ExitAfterAbort = true;
		return;
	}
	elog(FATAL, "The system is shutting down");
}

/* signal handler for floating point exception */
void
FloatExceptionHandler(SIGNAL_ARGS)
{
	elog(ERROR, "floating point exception!"
		 " The last floating point operation either exceeded legal ranges"
		 " or was a divide by zero");
}

/* signal handler for query cancel signal from postmaster */
static void
QueryCancelHandler(SIGNAL_ARGS)
{
	QueryCancel = true;
	LockWaitCancel();
}

void
CancelQuery(void)
{

	/*
	 * QueryCancel flag will be reset in main loop, which we reach by
	 * longjmp from elog().
	 */
	elog(ERROR, "Query was cancelled.");
}


static void
usage(char *progname)
{
	fprintf(stderr,
			"Usage: %s [options] [dbname]\n", progname);
#ifdef USE_ASSERT_CHECKING
	fprintf(stderr, "\t-A on\t\tenable/disable assert checking\n");
#endif
	fprintf(stderr, "\t-B buffers\tset number of buffers in buffer pool\n");
	fprintf(stderr, "\t-C \t\tsuppress version info\n");
	fprintf(stderr, "\t-D dir\t\tdata directory\n");
	fprintf(stderr, "\t-E \t\techo query before execution\n");
	fprintf(stderr, "\t-F \t\tturn off fsync\n");
#ifdef LOCK_MGR_DEBUG
	fprintf(stderr, "\t-K lev\t\tset locking debug level [0|1|2]\n");
#endif
	fprintf(stderr, "\t-L \t\tturn off locking\n");
	fprintf(stderr, "\t-N \t\tdon't use newline as interactive query delimiter\n");
	fprintf(stderr, "\t-O \t\tallow system table structure changes\n");
	fprintf(stderr, "\t-Q \t\tsuppress informational messages\n");
	fprintf(stderr, "\t-S kbytes\tset amount of memory for sorts (in kbytes)\n");
	fprintf(stderr, "\t-T options\tspecify pg_options\n");
	fprintf(stderr, "\t-W sec\t\twait N seconds to allow attach from a debugger\n");
	fprintf(stderr, "\t-d [1-5]\tset debug level\n");
	fprintf(stderr, "\t-e \t\tturn on European date format\n");
	fprintf(stderr, "\t-f [s|i|n|m|h]\tforbid use of some plan types\n");
	fprintf(stderr, "\t-i \t\tdon't execute queries\n");
	fprintf(stderr, "\t-o file\t\tsend stdout and stderr to given filename\n");
	fprintf(stderr, "\t-p database\tbackend is started under a postmaster\n");
	fprintf(stderr, "\t-s \t\tshow stats after each query\n");
	fprintf(stderr, "\t-t [pa|pl|ex]\tshow timings after each query\n");
	fprintf(stderr, "\t-v version\tset protocol version being used by frontend\n");
}

/* ----------------------------------------------------------------
 *	PostgresMain
 *		postgres main loop
 *		all backends, interactive or otherwise start here
 *
 *	argc/argv are the command line arguments to be used.  When being forked
 *	by the postmaster, these are not the original argv array of the process.
 *	real_argc/real_argv point to the original argv array, which is needed by
 *	PS_INIT_STATUS on some platforms.
 * ----------------------------------------------------------------
 */
int
PostgresMain(int argc, char *argv[], int real_argc, char *real_argv[])
{
	int			flag;

	char	   *DBName = NULL;
	bool		secure = true;
	int			errs = 0;

	int			firstchar;
	StringInfo	parser_input;
	char	   *userName;

	/* Used if verbose is set, must be initialized */
	char	   *remote_info = "interactive";
	char	   *remote_host = "";
	unsigned short remote_port = 0;

	extern int	optind;
	extern char *optarg;
	extern int	DebugLvl;

	/*
	 * Set default values for command-line options.
	 */
	IsUnderPostmaster = false;
	ShowStats = 0;
	ShowParserStats = ShowPlannerStats = ShowExecutorStats = 0;
	DeadlockCheckTimer = DEADLOCK_CHECK_TIMER;
	Noversion = false;
	EchoQuery = false;
#ifdef LOCK_MGR_DEBUG
	LockDebug = 0;
#endif
	DataDir = getenv("PGDATA");

	SetProcessingMode(InitProcessing);

	/* Check for PGDATESTYLE environment variable */
	set_default_datestyle();

	/*
	 * Read default pg_options from file $DATADIR/pg_options.
	 */
	if (DataDir)
		read_pg_options(0);

	/* ----------------
	 *	parse command line arguments
	 *
	 *	There are now two styles of command line layout for the backend:
	 *
	 *	For interactive use (not started from postmaster) the format is
	 *		postgres [switches] [databasename]
	 *	If the databasename is omitted it is taken to be the user name.
	 *
	 *	When started from the postmaster, the format is
	 *		postgres [secure switches] -p databasename [insecure switches]
	 *	Switches appearing after -p came from the client (via "options"
	 *	field of connection request).  For security reasons we restrict
	 *	what these switches can do.
	 * ----------------
	 */

	optind = 1;					/* reset after postmaster's usage */

	while ((flag = getopt(argc, argv,
						  "A:B:CD:d:EeFf:iK:LNOPo:p:QS:sT:t:v:W:x:"))
		   != EOF)
		switch (flag)
		{
			case 'A':
				/* ----------------
				 *	enable/disable assert checking.
				 * ----------------
				 */
#ifdef USE_ASSERT_CHECKING
				assert_enabled = atoi(optarg);
#else
				fprintf(stderr, "Assert checking is not enabled\n");
#endif
				break;

			case 'B':
				/* ----------------
				 *	specify the size of buffer pool
				 * ----------------
				 */
				if (secure)
					NBuffers = atoi(optarg);
				break;

			case 'C':
				/* ----------------
				 *	don't print version string
				 * ----------------
				 */
				Noversion = true;
				break;

			case 'D':			/* PGDATA directory */
				if (secure)
				{
					if (!DataDir)
					{
						DataDir = optarg;
						/* must be done after DataDir is defined */
						read_pg_options(0);
					}
					DataDir = optarg;
				}
				break;

			case 'd':			/* debug level */
				DebugLvl = atoi(optarg);
				if (DebugLvl >= 1)
					Verbose = true;
				if (DebugLvl >= 2)
					DebugPrintQuery = true;
				if (DebugLvl >= 3)
					DebugPrintParse = true;
				if (DebugLvl >= 4)
					DebugPrintPlan = true;
				if (DebugLvl >= 5)
					DebugPPrintRewrittenParsetree = true;
				break;

			case 'E':
				/* ----------------
				 *	E - echo the query the user entered
				 * ----------------
				 */
				EchoQuery = true;
				break;

			case 'e':
				/* --------------------------
				 * Use european date formats.
				 * --------------------------
				 */
				EuroDates = true;
				break;

			case 'F':
				/* --------------------
				 *	turn off fsync
				 *
				 *	7.0 buffer manager can support different backends running
				 *	with different fsync settings, so this no longer needs
				 *	to be "if (secure)".
				 * --------------------
				 */
				disableFsync = true;
				break;

			case 'f':
				/* -----------------
				 *	  f - forbid generation of certain plans
				 * -----------------
				 */
				switch (optarg[0])
				{
					case 's':	/* seqscan */
						enable_seqscan = false;
						break;
					case 'i':	/* indexscan */
						enable_indexscan = false;
						break;
					case 't':	/* tidscan */
						enable_tidscan = false;
						break;
					case 'n':	/* nestloop */
						enable_nestloop = false;
						break;
					case 'm':	/* mergejoin */
						enable_mergejoin = false;
						break;
					case 'h':	/* hashjoin */
						enable_hashjoin = false;
						break;
					default:
						errs++;
				}
				break;

			case 'i':
				dontExecute = 1;
				break;

			case 'K':
#ifdef LOCK_MGR_DEBUG
				LockDebug = atoi(optarg);
#else
				fprintf(stderr, "Lock debug not compiled in\n");
#endif
				break;

			case 'L':
				/* --------------------
				 *	turn off locking
				 * --------------------
				 */
				if (secure)
					lockingOff = 1;
				break;

			case 'N':
				/* ----------------
				 *	N - Don't use newline as a query delimiter
				 * ----------------
				 */
				UseNewLine = 0;
				break;

			case 'O':
				/* --------------------
				 *	allow system table structure modifications
				 * --------------------
				 */
				if (secure)		/* XXX safe to allow from client??? */
					allowSystemTableMods = true;
				break;

			case 'P':
				/* --------------------
				 *	ignore system indexes
				 * --------------------
				 */
				if (secure)		/* XXX safe to allow from client??? */
					IgnoreSystemIndexes(true);
				break;

			case 'o':
				/* ----------------
				 *	o - send output (stdout and stderr) to the given file
				 * ----------------
				 */
				if (secure)
					StrNCpy(OutputFileName, optarg, MAXPGPATH);
				break;

			case 'p':
				/* ----------------
				 *	p - special flag passed if backend was forked
				 *		by a postmaster.
				 * ----------------
				 */
				if (secure)
				{
					IsUnderPostmaster = true;
					DBName = optarg;
					secure = false;		/* subsequent switches are NOT
										 * secure */
				}
				break;

			case 'Q':
				/* ----------------
				 *	Q - set Quiet mode (reduce debugging output)
				 * ----------------
				 */
				Verbose = false;
				break;

			case 'S':
				/* ----------------
				 *	S - amount of sort memory to use in 1k bytes
				 * ----------------
				 */
				{
					int			S;

					S = atoi(optarg);
					if (S >= 4 * BLCKSZ / 1024)
						SortMem = S;
				}
				break;

			case 's':
				/* ----------------
				 *	  s - report usage statistics (timings) after each query
				 * ----------------
				 */
				ShowStats = 1;
				StatFp = stderr;
				break;

			case 'T':
				/* ----------------
				 *	T - tracing options
				 * ----------------
				 */
				parse_options(optarg, secure);
				break;

			case 't':
				/* ----------------
				 *	tell postgres to report usage statistics (timings) for
				 *	each query
				 *
				 *	-tpa[rser] = print stats for parser time of each query
				 *	-tpl[anner] = print stats for planner time of each query
				 *	-te[xecutor] = print stats for executor time of each query
				 *	caution: -s can not be used together with -t.
				 * ----------------
				 */
				StatFp = stderr;
				switch (optarg[0])
				{
					case 'p':
						if (optarg[1] == 'a')
							ShowParserStats = 1;
						else if (optarg[1] == 'l')
							ShowPlannerStats = 1;
						else
							errs++;
						break;
					case 'e':
						ShowExecutorStats = 1;
						break;
					default:
						errs++;
						break;
				}
				break;

			case 'v':
				if (secure)
					FrontendProtocol = (ProtocolVersion) atoi(optarg);
				break;

			case 'W':
				/* ----------------
				 *	wait N seconds to allow attach from a debugger
				 * ----------------
				 */
				sleep(atoi(optarg));
				break;

			case 'x':
#ifdef NOT_USED					/* planner/xfunc.h */

				/*
				 * control joey hellerstein's expensive function
				 * optimization
				 */
				if (XfuncMode != 0)
				{
					fprintf(stderr, "only one -x flag is allowed\n");
					errs++;
					break;
				}
				if (strcmp(optarg, "off") == 0)
					XfuncMode = XFUNC_OFF;
				else if (strcmp(optarg, "nor") == 0)
					XfuncMode = XFUNC_NOR;
				else if (strcmp(optarg, "nopull") == 0)
					XfuncMode = XFUNC_NOPULL;
				else if (strcmp(optarg, "nopm") == 0)
					XfuncMode = XFUNC_NOPM;
				else if (strcmp(optarg, "pullall") == 0)
					XfuncMode = XFUNC_PULLALL;
				else if (strcmp(optarg, "wait") == 0)
					XfuncMode = XFUNC_WAIT;
				else
				{
					fprintf(stderr, "use -x {off,nor,nopull,nopm,pullall,wait}\n");
					errs++;
				}
#endif
				break;

			default:
				/* ----------------
				 *	default: bad command line option
				 * ----------------
				 */
				errs++;
				break;
		}

	if (ShowStats &&
		(ShowParserStats || ShowPlannerStats || ShowExecutorStats))
	{
		fprintf(stderr, "-s can not be used together with -t.\n");
		proc_exit(0);
	}

	if (!DataDir)
	{
		fprintf(stderr, "%s does not know where to find the database system "
				"data.  You must specify the directory that contains the "
				"database system either by specifying the -D invocation "
			 "option or by setting the PGDATA environment variable.\n\n",
				argv[0]);
		proc_exit(1);
	}

	/*
	 * 1. Set BlockSig and UnBlockSig masks. 2. Set up signal handlers. 3.
	 * Allow only SIGUSR1 signal (we never block it) during
	 * initialization.
	 *
	 * Note that postmaster already blocked ALL signals to make us happy.
	 */
	if (!IsUnderPostmaster)
	{
		PG_INITMASK();
		PG_SETMASK(&BlockSig);
	}

#ifdef HAVE_SIGPROCMASK
	sigdelset(&BlockSig, SIGUSR1);
#else
	BlockSig &= ~(sigmask(SIGUSR1));
#endif

	pqsignal(SIGHUP, read_pg_options);	/* update pg_options from file */
	pqsignal(SIGINT, QueryCancelHandler);		/* cancel current query */
	pqsignal(SIGQUIT, handle_warn);		/* handle error */
	pqsignal(SIGTERM, die);
	pqsignal(SIGALRM, HandleDeadLock);

	/*
	 * Ignore failure to write to frontend. Note: if frontend closes
	 * connection, we will notice it and exit cleanly when control next
	 * returns to outer loop.  This seems safer than forcing exit in the
	 * midst of output during who-knows-what operation...
	 */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, quickdie);
	pqsignal(SIGUSR2, Async_NotifyHandler);		/* flush also sinval cache */
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_IGN); /* ignored, sent by LockOwners */
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);

	PG_SETMASK(&BlockSig);		/* block everything except SIGUSR1 */

	/*
	 * Get user name (needed now in case it is the default database name)
	 * and check command line validity
	 */
	SetPgUserName();
	userName = GetPgUserName();

	if (IsUnderPostmaster)
	{
		/* noninteractive case: nothing should be left after switches */
		if (errs || argc != optind || DBName == NULL)
		{
			usage(argv[0]);
			proc_exit(0);
		}
		pq_init();				/* initialize libpq at backend startup */
		whereToSendOutput = Remote;
		BaseInit();
	}
	else
	{
		/* interactive case: database name can be last arg on command line */
		whereToSendOutput = Debug;
		if (errs || argc - optind > 1)
		{
			usage(argv[0]);
			proc_exit(0);
		}
		else if (argc - optind == 1)
			DBName = argv[optind];
		else if ((DBName = userName) == NULL)
		{
			fprintf(stderr, "%s: USER undefined and no database specified\n",
					argv[0]);
			proc_exit(0);
		}

		/*
		 * Try to create pid file.
		 */
		SetPidFname(DataDir);
		if (SetPidFile(-getpid()))
			proc_exit(0);

		/*
		 * Register clean up proc.
		 */
		on_proc_exit(UnlinkPidFile, NULL);

		BaseInit();
		snprintf(XLogDir, MAXPGPATH, "%s%cpg_xlog",
				 DataDir, SEP_CHAR);
		snprintf(ControlFilePath, MAXPGPATH, "%s%cpg_control",
				 DataDir, SEP_CHAR);
		StartupXLOG();
	}

	/*
	 * Set up additional info.
	 */

#ifdef CYR_RECODE
	SetCharSet();
#endif

	/* On some systems our dynloader code needs the executable's pathname */
	if (FindExec(pg_pathname, argv[0], "postgres") < 0)
		elog(FATAL, "%s: could not locate executable, bailing out...",
			 argv[0]);

	/*
	 * Find remote host name or address.
	 */
	if (IsUnderPostmaster)
	{
		switch (MyProcPort->raddr.sa.sa_family)
		{
				struct hostent *host_ent;

			case AF_INET:
				remote_info = remote_host = malloc(48);
				remote_port = ntohs(MyProcPort->raddr.in.sin_port);
				strcpy(remote_host, inet_ntoa(MyProcPort->raddr.in.sin_addr));
				if (HostnameLookup)
				{
					host_ent = \
						gethostbyaddr((char *) &MyProcPort->raddr.in.sin_addr,
								   sizeof(MyProcPort->raddr.in.sin_addr),
									  AF_INET);
					if (host_ent)
					{
						strncpy(remote_host, host_ent->h_name, 48);
						*(remote_host + 47) = '\0';
					}
				}
				if (ShowPortNumber)
				{
					remote_info = malloc(strlen(remote_host) + 6);
					sprintf(remote_info, "%s:%d", remote_host, remote_port);
				}
				break;
			case AF_UNIX:
				remote_info = remote_host = "localhost";
				break;
			default:
				remote_info = remote_host = "unknown";
				break;
		}

		/*
		 * Set process params for ps
		 */
		PS_INIT_STATUS(real_argc, real_argv, argv[0],
					   remote_info, userName, DBName);
		PS_SET_STATUS("startup");
	}


	/* ----------------
	 *	print flags
	 * ----------------
	 */
	if (Verbose)
	{
		if (Verbose)
		{
			TPRINTF(TRACE_VERBOSE, "started: host=%s user=%s database=%s",
					remote_host, userName, DBName);
		}
		else
		{
			TPRINTF(TRACE_VERBOSE, "debug info:");
			TPRINTF(TRACE_VERBOSE, "\tUser         = %s", userName);
			TPRINTF(TRACE_VERBOSE, "\tRemoteHost   = %s", remote_host);
			TPRINTF(TRACE_VERBOSE, "\tRemotePort   = %d", remote_port);
			TPRINTF(TRACE_VERBOSE, "\tDatabaseName = %s", DBName);
			TPRINTF(TRACE_VERBOSE, "\tDebug Level  = %d", DebugLvl);
			TPRINTF(TRACE_VERBOSE, "\tNoversion    = %c", Noversion ? 't' : 'f');
			TPRINTF(TRACE_VERBOSE, "\ttimings      = %c", ShowStats ? 't' : 'f');
			TPRINTF(TRACE_VERBOSE, "\tdates        = %s",
					EuroDates ? "European" : "Normal");
			TPRINTF(TRACE_VERBOSE, "\tbufsize      = %d", NBuffers);
			TPRINTF(TRACE_VERBOSE, "\tsortmem      = %d", SortMem);
			TPRINTF(TRACE_VERBOSE, "\tquery echo   = %c", EchoQuery ? 't' : 'f');
		}
	}


	/*
	 * general initialization
	 */

	if (Verbose)
		TPRINTF(TRACE_VERBOSE, "InitPostgres");

	InitPostgres(DBName);

#ifdef MULTIBYTE
	/* set default client encoding */
	if (Verbose)
		TPRINTF(TRACE_VERBOSE, "reset_client_encoding()..");
	reset_client_encoding();
	if (Verbose)
		TPRINTF(TRACE_VERBOSE, "reset_client_encoding() done.");
#endif

	on_shmem_exit(remove_all_temp_relations, NULL);

	{
		MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		parser_input = makeStringInfo();		/* initialize input buffer */
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * Send this backend's cancellation info to the frontend.
	 */
	if (whereToSendOutput == Remote &&
		PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		StringInfoData buf;

		pq_beginmessage(&buf);
		pq_sendbyte(&buf, 'K');
		pq_sendint(&buf, (int32) MyProcPid, sizeof(int32));
		pq_sendint(&buf, (int32) MyCancelKey, sizeof(int32));
		pq_endmessage(&buf);
		/* Need not flush since ReadyForQuery will do it. */
	}

	if (!IsUnderPostmaster)
	{
		puts("\nPOSTGRES backend interactive interface ");
		puts("$Revision: 1.152 $ $Date: 2000/04/23 00:13:16 $\n");
	}

	/*
	 * Initialize the deferred trigger manager
	 */
	if (DeferredTriggerInit() != 0)
		proc_exit(0);

	SetProcessingMode(NormalProcessing);

	/*
	 * POSTGRES main processing loop begins here
	 *
	 * If an exception is encountered, processing resumes here so we abort
	 * the current transaction and start a new one.
	 */

	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		time(&tim);

		if (Verbose)
			TPRINTF(TRACE_VERBOSE, "AbortCurrentTransaction");

		AbortCurrentTransaction();
		InError = false;
		if (ExitAfterAbort)
		{
			ProcReleaseLocks(); /* Just to be sure... */
			proc_exit(0);
		}
	}

	Warn_restart_ready = true;	/* we can now handle elog(ERROR) */

	PG_SETMASK(&UnBlockSig);

	/*
	 * Non-error queries loop here.
	 */

	for (;;)
	{
		PS_SET_STATUS("idle");

		/* ----------------
		 *	 (1) tell the frontend we're ready for a new query.
		 *
		 *	 Note: this includes fflush()'ing the last of the prior output.
		 * ----------------
		 */
		ReadyForQuery(whereToSendOutput);

		/* ----------------
		 *	 (2) deal with pending asynchronous NOTIFY from other backends,
		 *	 and enable async.c's signal handler to execute NOTIFY directly.
		 * ----------------
		 */
		QueryCancel = false;	/* forget any earlier CANCEL signal */
		SetWaitingForLock(false);

		EnableNotifyInterrupt();

		/* ----------------
		 *	 (3) read a command.
		 * ----------------
		 */
		firstchar = ReadCommand(parser_input);

		QueryCancel = false;	/* forget any earlier CANCEL signal */

		/* ----------------
		 *	 (4) disable async.c's signal handler.
		 * ----------------
		 */
		DisableNotifyInterrupt();

		/* ----------------
		 *	 (5) process the command.
		 * ----------------
		 */
		switch (firstchar)
		{
				/* ----------------
				 *	'F' indicates a fastpath call.
				 *		XXX HandleFunctionRequest
				 * ----------------
				 */
			case 'F':
				IsEmptyQuery = false;

				/* start an xact for this function invocation */
				if (Verbose)
					TPRINTF(TRACE_VERBOSE, "StartTransactionCommand");
				StartTransactionCommand();

				if (HandleFunctionRequest() == EOF)
				{
					/* lost frontend connection during F message input */
					pq_close();
					proc_exit(0);
				}
				break;

				/* ----------------
				 *	'Q' indicates a user query
				 * ----------------
				 */
			case 'Q':
				if (strspn(parser_input->data, " \t\n") == parser_input->len)
				{
					/* ----------------
					 *	if there is nothing in the input buffer, don't bother
					 *	trying to parse and execute anything..
					 * ----------------
					 */
					IsEmptyQuery = true;
				}
				else
				{
					/* ----------------
					 *	otherwise, process the input string.
					 * ----------------
					 */
					IsEmptyQuery = false;
					if (ShowStats)
						ResetUsage();

					/* start an xact for this query */
					if (Verbose)
						TPRINTF(TRACE_VERBOSE, "StartTransactionCommand");
					StartTransactionCommand();

					pg_exec_query(parser_input->data);

					/*
					 * Invoke IMMEDIATE constraint triggers
					 *
					 */
					DeferredTriggerEndQuery();

					if (ShowStats)
						ShowUsage();
				}
				break;

				/* ----------------
				 *	'X' means that the frontend is closing down the socket.
				 *	EOF means unexpected loss of frontend connection.
				 *	Either way, perform normal shutdown.
				 * ----------------
				 */
			case 'X':
			case EOF:
				if (!IsUnderPostmaster)
					ShutdownXLOG();
				pq_close();
				proc_exit(0);
				break;

			default:
				elog(ERROR, "unknown frontend message was received");
		}

		/* ----------------
		 *	 (6) commit the current transaction
		 *
		 *	 Note: if we had an empty input buffer, then we didn't
		 *	 call pg_exec_query, so we don't bother to commit this transaction.
		 * ----------------
		 */
		if (!IsEmptyQuery)
		{
			if (Verbose)
				TPRINTF(TRACE_VERBOSE, "CommitTransactionCommand");
			PS_SET_STATUS("commit");
			CommitTransactionCommand();
		}
		else
		{
			if (IsUnderPostmaster)
				NullCommand(Remote);
		}
	}							/* infinite for-loop */

	proc_exit(0);				/* shouldn't get here... */
	return 1;
}

#ifndef HAVE_GETRUSAGE
#include "rusagestub.h"
#else
#include <sys/resource.h>
#endif	 /* HAVE_GETRUSAGE */

struct rusage Save_r;
struct timeval Save_t;

void
ResetUsage(void)
{
	struct timezone tz;

	getrusage(RUSAGE_SELF, &Save_r);
	gettimeofday(&Save_t, &tz);
	ResetBufferUsage();
/*	  ResetTupleCount(); */
}

void
ShowUsage(void)
{
	struct timeval user,
				sys;
	struct timeval elapse_t;
	struct timezone tz;
	struct rusage r;

	getrusage(RUSAGE_SELF, &r);
	gettimeofday(&elapse_t, &tz);
	memmove((char *) &user, (char *) &r.ru_utime, sizeof(user));
	memmove((char *) &sys, (char *) &r.ru_stime, sizeof(sys));
	if (elapse_t.tv_usec < Save_t.tv_usec)
	{
		elapse_t.tv_sec--;
		elapse_t.tv_usec += 1000000;
	}
	if (r.ru_utime.tv_usec < Save_r.ru_utime.tv_usec)
	{
		r.ru_utime.tv_sec--;
		r.ru_utime.tv_usec += 1000000;
	}
	if (r.ru_stime.tv_usec < Save_r.ru_stime.tv_usec)
	{
		r.ru_stime.tv_sec--;
		r.ru_stime.tv_usec += 1000000;
	}

	/*
	 * the only stats we don't show here are for memory usage -- i can't
	 * figure out how to interpret the relevant fields in the rusage
	 * struct, and they change names across o/s platforms, anyway. if you
	 * can figure out what the entries mean, you can somehow extract
	 * resident set size, shared text size, and unshared data and stack
	 * sizes.
	 */

	fprintf(StatFp, "! system usage stats:\n");
	fprintf(StatFp,
			"!\t%ld.%06ld elapsed %ld.%06ld user %ld.%06ld system sec\n",
			(long int) elapse_t.tv_sec - Save_t.tv_sec,
			(long int) elapse_t.tv_usec - Save_t.tv_usec,
			(long int) r.ru_utime.tv_sec - Save_r.ru_utime.tv_sec,
			(long int) r.ru_utime.tv_usec - Save_r.ru_utime.tv_usec,
			(long int) r.ru_stime.tv_sec - Save_r.ru_stime.tv_sec,
			(long int) r.ru_stime.tv_usec - Save_r.ru_stime.tv_usec);
	fprintf(StatFp,
			"!\t[%ld.%06ld user %ld.%06ld sys total]\n",
			(long int) user.tv_sec,
			(long int) user.tv_usec,
			(long int) sys.tv_sec,
			(long int) sys.tv_usec);
#ifdef HAVE_GETRUSAGE
	fprintf(StatFp,
			"!\t%ld/%ld [%ld/%ld] filesystem blocks in/out\n",
			r.ru_inblock - Save_r.ru_inblock,
	/* they only drink coffee at dec */
			r.ru_oublock - Save_r.ru_oublock,
			r.ru_inblock, r.ru_oublock);
	fprintf(StatFp,
		  "!\t%ld/%ld [%ld/%ld] page faults/reclaims, %ld [%ld] swaps\n",
			r.ru_majflt - Save_r.ru_majflt,
			r.ru_minflt - Save_r.ru_minflt,
			r.ru_majflt, r.ru_minflt,
			r.ru_nswap - Save_r.ru_nswap,
			r.ru_nswap);
	fprintf(StatFp,
	 "!\t%ld [%ld] signals rcvd, %ld/%ld [%ld/%ld] messages rcvd/sent\n",
			r.ru_nsignals - Save_r.ru_nsignals,
			r.ru_nsignals,
			r.ru_msgrcv - Save_r.ru_msgrcv,
			r.ru_msgsnd - Save_r.ru_msgsnd,
			r.ru_msgrcv, r.ru_msgsnd);
	fprintf(StatFp,
		 "!\t%ld/%ld [%ld/%ld] voluntary/involuntary context switches\n",
			r.ru_nvcsw - Save_r.ru_nvcsw,
			r.ru_nivcsw - Save_r.ru_nivcsw,
			r.ru_nvcsw, r.ru_nivcsw);
#endif	 /* HAVE_GETRUSAGE */
	fprintf(StatFp, "! postgres usage stats:\n");
	PrintBufferUsage(StatFp);
/*	   DisplayTupleCount(StatFp); */
}

#ifdef USE_ASSERT_CHECKING
int
assertEnable(int val)
{
	assert_enabled = val;
	return val;
}

#ifdef ASSERT_CHECKING_TEST
int
assertTest(int val)
{
	Assert(val == 0);

	if (assert_enabled)
	{
		/* val != 0 should be trapped by previous Assert */
		elog(NOTICE, "Assert test successfull (val = %d)", val);
	}
	else
		elog(NOTICE, "Assert checking is disabled (val = %d)", val);

	return val;
}

#endif
#endif
