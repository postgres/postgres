/*-------------------------------------------------------------------------
 *
 * postgres.c
 *	  POSTGRES C Backend Interface
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/postgres.c,v 1.99 1999/02/13 23:18:45 momjian Exp $
 *
 * NOTES
 *	  this is the "main" module of the postgres backend and
 *	  hence the main module of the "traffic cop".
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/param.h>			/* for MAXHOSTNAMELEN on most */
#ifndef MAXHOSTNAMELEN
#include <netdb.h>				/* for MAXHOSTNAMELEN on some */
#endif
#ifndef MAXHOSTNAMELEN			/* for MAXHOSTNAMELEN under sco3.2v5.0.2 */
#include <sys/socket.h>
#endif
#include <errno.h>
#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif	 /* aix */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef __CYGWIN32__
#include <getopt.h>
#endif

#include "postgres.h"
#include "miscadmin.h"
#include "fmgr.h"

#include "access/xact.h"
#include "catalog/catname.h"
#include "commands/async.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqsignal.h"
#include "nodes/pg_list.h"
#include "nodes/print.h"
#include "optimizer/cost.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"		/* for QueryRewrite() */
#include "storage/bufmgr.h"
#include "tcop/dest.h"
#include "tcop/fastpath.h"
#include "tcop/pquery.h"
#include "tcop/tcopdebug.h"
#include "tcop/tcopprot.h"		/* where declarations for this file go */
#include "tcop/utility.h"
#include "utils/mcxt.h"
#include "utils/rel.h"
#include "utils/ps_status.h"
#include "utils/temprel.h"

#if FALSE
#include "nodes/relation.h"
#endif

#if 0
#include "optimizer/xfunc.h"
#endif

#if FALSE
#include "nodes/plannodes.h"
#endif

#if FALSE
#include "nodes/memnodes.h"
#endif

#include "utils/trace.h"

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
#define ShowParserStats		pg_options[TRACE_PARSERSTATS]
#define ShowPlannerStats	pg_options[TRACE_PLANNERSTATS]
#define ShowExecutorStats	pg_options[TRACE_EXECUTORSTATS]
#define DebugPrintRewrittenParsetree \
							pg_options[TRACE_REWRITTEN]
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

/*static bool	EnableRewrite = true; , never changes why have it*/
CommandDest whereToSendOutput;

/* Define status buffer needed by PS_SET_STATUS */
PS_DEFINE_BUFFER;

extern int	lockingOff;
extern int	NBuffers;

int			dontExecute = 0;
static int	ShowStats;
static bool IsEmptyQuery = false;

char		relname[80];		/* current relation name */

#if defined(nextstep)
jmp_buf		Warn_restart;

#define sigsetjmp(x,y)	setjmp(x)
#define siglongjmp longjmp
#else
sigjmp_buf	Warn_restart;

#endif	 /* defined(nextstep) */
bool		InError;

extern int	NBuffers;

static int	EchoQuery = 0;		/* default don't echo */
time_t		tim;
char		pg_pathname[256];
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

/* ----------------
 *		bushy tree plan flag: if true planner will generate bushy-tree
 *		plans
 * ----------------
 */
int			BushyPlanFlag = 0;	/* default to false -- consider only
								 * left-deep trees */

/*
** Flags for expensive function optimization -- JMH 3/9/92
*/
int			XfuncMode = 0;

/*
 * ----------------
 *	 Note: _exec_repeat_ defaults to 1 but may be changed
 *		   by a DEBUG command.	 If you set this to a large
 *		   number N, run a single query, and then set it
 *		   back to 1 and run N queries, you can get an idea
 *		   of how much time is being spent in the parser and
 *		   planner b/c in the first case this overhead only
 *		   happens once.  -cim 6/9/91
 * ----------------
*/
int			_exec_repeat_ = 1;

/* ----------------------------------------------------------------
 *		decls for routines only used in this file
 * ----------------------------------------------------------------
 */
static char InteractiveBackend(char *inBuf);
static char SocketBackend(char *inBuf);
static char ReadCommand(char *inBuf);


/* ----------------------------------------------------------------
 *		routines to obtain user input
 * ----------------------------------------------------------------
 */

/* ----------------
 *	InteractiveBackend() is called for user interactive connections
 *	the string entered by the user is placed in its parameter inBuf.
 * ----------------
 */

static char
InteractiveBackend(char *inBuf)
{
	char	   *stuff = inBuf;	/* current place in input buffer */
	int			c;				/* character read from getc() */
	bool		end = false;	/* end-of-input flag */
	bool		backslashSeen = false;	/* have we seen a \ ? */

	/* ----------------
	 *	display a prompt and obtain input from the user
	 * ----------------
	 */
	printf("> ");
	fflush(stdout);

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
						stuff--;
						continue;
					}
					else
					{
						/* keep the newline character */
						*stuff++ = '\n';
						*stuff++ = '\0';
						break;
					}
				}
				else if (c == '\\')
					backslashSeen = true;
				else
					backslashSeen = false;

				*stuff++ = (char) c;
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
				*stuff++ = (char) c;

			if (stuff == inBuf)
				end = true;
		}

		if (end)
		{
			if (Verbose)
				puts("EOF");
			IsEmptyQuery = true;
			proc_exit(0);
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
		printf("query: %s\n", inBuf);
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
 *	the function call is processed in HandleFunctionRequest().
 *	(now called from PostgresMain())
 * ----------------
 */

static char
SocketBackend(char *inBuf)
{
	char		qtype[2];
	char		result = '\0';

	/* ----------------
	 *	get input from the frontend
	 * ----------------
	 */
	strcpy(qtype, "?");
	if (pq_getnchar(qtype, 0, 1) == EOF)
	{
		/* ------------
		 *	when front-end applications quits/dies
		 * ------------
		 */
		proc_exit(0);
	}

	switch (*qtype)
	{
			/* ----------------
			 *	'Q': user entered a query
			 * ----------------
			 */
		case 'Q':
			pq_getstr(inBuf, MAX_PARSE_BUFFER);
			result = 'Q';
			break;

			/* ----------------
			 *	'F':  calling user/system functions
			 * ----------------
			 */
		case 'F':
			pq_getstr(inBuf, MAX_PARSE_BUFFER); /* ignore the rest of the
												 * line */
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
			elog(FATAL, "Socket command type %c unknown\n", *qtype);
			break;
	}
	return result;
}

/* ----------------
 *		ReadCommand reads a command from either the frontend or
 *		standard input, places it in inBuf, and returns a char
 *		representing whether the string is a 'Q'uery or a 'F'astpath
 *		call.
 * ----------------
 */
static char
ReadCommand(char *inBuf)
{
	if (IsUnderPostmaster)
		return SocketBackend(inBuf);
	else
		return InteractiveBackend(inBuf);
}

List *
pg_parse_and_plan(char *query_string,	/* string to execute */
				  Oid *typev,	/* argument types */
				  int nargs,	/* number of arguments */
				  QueryTreeList **queryListP,	/* pointer to the parse
												 * trees */
				  CommandDest dest,		/* where results should go */
				  bool aclOverride)
{
	QueryTreeList *querytree_list;
	int			i;
	List	   *plan_list = NIL;
	Plan	   *plan;
	int			j;
	QueryTreeList *new_list;
	List	   *rewritten = NIL;
	Query	   *querytree;

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

	/* new_list holds the rewritten queries */
	new_list = (QueryTreeList *) malloc(sizeof(QueryTreeList));
	new_list->len = querytree_list->len;
	new_list->qtrees = (Query **) malloc(new_list->len * sizeof(Query *));

	/* ----------------
	 *	(2) rewrite the queries, as necessary
	 * ----------------
	 */
	j = 0;						/* counter for the new_list, new_list can
								 * be longer than old list as a result of
								 * rewrites */
	for (i = 0; i < querytree_list->len; i++)
	{
		List	   *union_result,
				   *union_list,
				   *rewritten_list;

		querytree = querytree_list->qtrees[i];

 		/***S*I***/
 		/* Rewrite Union, Intersect and Except Queries
 		 * to normal Union Queries using IN and NOT IN subselects */
 		if(querytree->intersectClause != NIL) 
 		  {	  
 		    querytree = Except_Intersect_Rewrite(querytree);
 		  }

		if (DebugPrintQuery)
		{
			if (DebugPrintQuery > 3)
			{			  
			  /* Print the query string as is if query debug level > 3 */
			  TPRINTF(TRACE_QUERY, "query: %s", query_string); 
			}
			else
			{
				/* Print condensed query string to fit in one log line */
				char		buff[8192 + 1];
				char		c,
						   *s,
						   *d;
				int			n,
							is_space = 1;

				for (s = query_string, d = buff, n = 0; (c = *s) && (n < 8192); s++)
				{
					switch (c)
					{
						case '\r':
						case '\n':
						case '\t':
							c = ' ';
							/* fall through */
						case ' ':
							if (is_space)
								continue;
							is_space = 1;
							break;
						default:
							is_space = 0;
							break;
					}
					*d++ = c;
					n++;
				}
				*d = '\0';
				TPRINTF(TRACE_QUERY, "query: %s", buff);
			}
		}

		/* don't rewrite utilites */
		if (querytree->commandType == CMD_UTILITY)
		{
			new_list->qtrees[j++] = querytree;
			continue;
		}

		if (DebugPrintParse)
		{
			TPRINTF(TRACE_PARSE, "parser outputs:");
			nodeDisplay(querytree);
		}

		/* rewrite queries (retrieve, append, delete, replace) */
		rewritten = QueryRewrite(querytree);

		if (rewritten != NIL)
		{
			int			len,
						k;

			/*
			 * Rewrite the UNIONS.
			 */
			foreach(rewritten_list, rewritten)
			{
				Query	   *qry = (Query *) lfirst(rewritten_list);

				union_result = NIL;
				foreach(union_list, qry->unionClause)
					union_result = nconc(union_result, QueryRewrite((Query *) lfirst(union_list)));
				qry->unionClause = union_result;
			}

			len = length(rewritten);
			if (len == 1)
				new_list->qtrees[j++] = (Query *) lfirst(rewritten);
			else
			{
				/* rewritten queries are longer than original query */
				/* grow the new_list to accommodate */
				new_list->len += len - 1;		/* - 1 because originally
												 * we allocated one space
												 * for the query */
				new_list->qtrees = realloc(new_list->qtrees,
										new_list->len * sizeof(Query *));
				for (k = 0; k < len; k++)
					new_list->qtrees[j++] = (Query *) nth(k, rewritten);
			}
		}
	}

	/* ----------
	 * Due to rewriting, the new list could also have been
	 * shrunk (do instead nothing). Forget obsolete queries
	 * at the end.
	 * ----------
	 */
	new_list->len = j;

	/* we're done with the original lists, free it */
	free(querytree_list->qtrees);
	free(querytree_list);

	querytree_list = new_list;

	/*
	 * Override ACL checking if requested
	 */
	if (aclOverride)
	{
		for (i = 0; i < querytree_list->len; i++)
		{
			RangeTblEntry *rte;
			List	   *l;

			querytree = querytree_list->qtrees[i];
			if (querytree->commandType == CMD_UTILITY)
				continue;

			foreach(l, querytree->rtable)
			{
				rte = (RangeTblEntry *) lfirst(l);

				rte->skipAcl = TRUE;
			}
		}
	}

	if (DebugPrintRewrittenParsetree)
	{
		TPRINTF(TRACE_REWRITTEN, "after rewriting:");

		for (i = 0; i < querytree_list->len; i++)
		{
			nodeDisplay(querytree_list->qtrees[i]);
			printf("\n");
		}
	}

	for (i = 0; i < querytree_list->len; i++)
	{
		querytree = querytree_list->qtrees[i];

		/*
		 * For each query that isn't a utility invocation, generate a
		 * plan.
		 */

		if (querytree->commandType != CMD_UTILITY)
		{

			if (IsAbortedTransactionBlockState())
			{
				/* ----------------
				 *	 the EndCommand() stuff is to tell the frontend
				 *	 that the command ended. -cim 6/1/90
				 * ----------------
				 */
				char	   *tag = "*ABORT STATE*";

				EndCommand(tag, dest);

				elog(NOTICE, "(transaction aborted): %s",
					 "queries ignored until END");

				free(querytree_list->qtrees);
				free(querytree_list);
				if (queryListP)
					*queryListP = (QueryTreeList *) NULL;
				return (List *) NULL;
			}

			if (ShowPlannerStats)
				ResetUsage();

			/* call that optimizer */
			plan = planner(querytree);

			if (ShowPlannerStats)
			{
				fprintf(stderr, "! Planner Stats:\n");
				ShowUsage();
			}
			plan_list = lappend(plan_list, plan);
#ifdef INDEXSCAN_PATCH
			/* ----------------
			 *	Print plan if debugging.
			 *	This has been moved here to get debugging output
			 *	also for queries in functions.	DZ - 27-8-1996
			 * ----------------
			 */
			if (DebugPrintPlan)
			{
				TPRINTF(TRACE_PLAN, "plan:");
				nodeDisplay(plan);
			}
#endif
		}
#ifdef FUNC_UTIL_PATCH

		/*
		 * If the command is an utility append a null plan. This is needed
		 * to keep the plan_list aligned with the querytree_list or the
		 * function executor will crash.  DZ - 30-8-1996
		 */
		else
			plan_list = lappend(plan_list, NULL);
#endif
	}

	/* ----------
	 * Check if the rewriting had thrown away anything
	 * ----------
	 */
	if (querytree_list->len == 0)
	{
		free(querytree_list->qtrees);
		free(querytree_list);
		querytree_list = NULL;
	}

	if (queryListP)
		*queryListP = querytree_list;

	return plan_list;
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

void
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
	List	   *plan_list;
	Plan	   *plan;
	Query	   *querytree;
	int			i,
				j;
	QueryTreeList *querytree_list;

	/* plan the queries */
	plan_list = pg_parse_and_plan(query_string, NULL, 0, &querytree_list, dest, aclOverride);

	if (QueryCancel)
		CancelQuery();

	/* pg_parse_and_plan could have failed */
	if (querytree_list == NULL)
		return;

	for (i = 0; i < querytree_list->len; i++)
	{
		querytree = querytree_list->qtrees[i];

#ifdef FUNC_UTIL_PATCH

		/*
		 * Advance on the plan_list in every case.	Now the plan_list has
		 * the same length of the querytree_list.  DZ - 30-8-1996
		 */
		plan = (Plan *) lfirst(plan_list);
		plan_list = lnext(plan_list);
#endif
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
#ifndef FUNC_UTIL_PATCH

			/*
			 * Moved before the if.  DZ - 30-8-1996
			 */
			plan = (Plan *) lfirst(plan_list);
			plan_list = lnext(plan_list);
#endif

#ifdef INDEXSCAN_PATCH

			/*
			 * Print moved in pg_parse_and_plan.	DZ - 27-8-1996
			 */
#else
			/* ----------------
			 *	print plan if debugging
			 * ----------------
			 */
			if (DebugPrintPlan)
			{
				TPRINTF(TRACE_PLAN, "plan:");
				nodeDisplay(plan);
			}
#endif

			SetQuerySnapshot();

			/*
			 *	 execute the plan
			 */
			if (ShowExecutorStats)
				ResetUsage();

			for (j = 0; j < _exec_repeat_; j++)
			{
				if (Verbose)
					TPRINTF(TRACE_VERBOSE, "ProcessQuery");
				ProcessQuery(querytree, plan, dest);
			}

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

		if (querytree_list)
			CommandCounterIncrement();
	}

	free(querytree_list->qtrees);
	free(querytree_list);
}

/* --------------------------------
 *		signal handler routines used in PostgresMain()
 *
 *		handle_warn() is used to catch kill(getpid(),SIGQUIT) which
 *		occurs when elog(ERROR) is called.
 *
 *		quickdie() occurs when signalled by the postmaster.
 *		Some backend has bought the farm,
 *		so we need to stop what we're doing and exit.
 *
 *		die() preforms an orderly cleanup via ExitPostgres()
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
	elog(NOTICE, "Message from PostgreSQL backend:"
		 "\n\tThe Postmaster has informed me that some other backend"
		 " died abnormally and possibly corrupted shared memory."
		 "\n\tI have rolled back the current transaction and am"
		 " going to terminate your database system connection and exit."
	"\n\tPlease reconnect to the database system and repeat your query.");


	/*
	 * DO NOT ExitPostgres(0) -- we're here because shared memory may be
	 * corrupted, so we don't want to flush any shared state to stable
	 * storage.  Just nail the windows shut and get out of town.
	 */

	exit(0);
}

void
die(SIGNAL_ARGS)
{
	ExitPostgres(0);
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
	fprintf(stderr, "    A: enable/disable assert checking\n");
#endif
	fprintf(stderr, "\t-B buffers\tset number of buffers in buffer pool\n");
	fprintf(stderr, "\t-C \t\tsupress version info\n");
	fprintf(stderr, "\t-D dir\t\tdata directory\n");
	fprintf(stderr, "\t-E \t\techo query before execution\n");
	fprintf(stderr, "\t-F \t\tturn off fsync\n");
#ifdef LOCK_MGR_DEBUG
	fprintf(stderr, "\t-K \t\tset locking debug level [0|1|2]\n");
#endif
	fprintf(stderr, "\t-P port\t\tset port file descriptor\n");
	fprintf(stderr, "\t-Q \t\tsuppress informational messages\n");
	fprintf(stderr, "\t-S buffers\tset amount of sort memory available\n");
	fprintf(stderr, "\t-d [1|2|3]\tset debug level\n");
	fprintf(stderr, "\t-e \t\tturn on European date format\n");
	fprintf(stderr, "\t-o file\t\tsend stdout and stderr to given filename \n");
	fprintf(stderr, "\t-s \t\tshow stats after each query\n");
	fprintf(stderr, "\t-v version\tset protocol version being used by frontend\n");
	fprintf(stderr, "\t-W \t\twait N seconds to allow attach from a debugger\n");
}

/* ----------------------------------------------------------------
 *		PostgresMain
 *		  postgres main loop
 *		all backends, interactive or otherwise start here
 * ----------------------------------------------------------------
 */
int
PostgresMain(int argc, char *argv[], int real_argc, char *real_argv[])
{
	bool		flagC = false,
				flagQ = false,
				flagE = false,
				flagEu = false;
	int			flag;

	char	   *DBName = NULL;
	int			errs = 0;

	char		firstchar;
	char		parser_input[MAX_PARSE_BUFFER];
	char	   *userName;

	/* Used if verbose is set, must be initialized */
	char	   *remote_info = "interactive";
	char	   *remote_host = "";
	unsigned short remote_port = 0;

	char	   *DBDate = NULL;
	extern int	optind;
	extern char *optarg;
	extern short DebugLvl;

	/* ----------------
	 *	parse command line arguments
	 * ----------------
	 */

	/*
	 * Set default values.
	 */
	ShowStats = 0;
	ShowParserStats = ShowPlannerStats = ShowExecutorStats = 0;
	DeadlockCheckTimer = DEADLOCK_CHECK_TIMER;
#ifdef LOCK_MGR_DEBUG
	LockDebug = 0;
#endif

	/*
	 * get hostname is either the environment variable PGHOST or NULL NULL
	 * means Unix-socket only
	 */
	DataDir = getenv("PGDATA");

	/*
	 * Try to get initial values for date styles and formats. Does not do
	 * a complete job, but should be good enough for backend. Cannot call
	 * parse_date() since palloc/pfree memory is not set up yet.
	 */
	DBDate = getenv("PGDATESTYLE");
	if (DBDate != NULL)
	{
		if (strcasecmp(DBDate, "ISO") == 0)
			DateStyle = USE_ISO_DATES;
		else if (strcasecmp(DBDate, "SQL") == 0)
			DateStyle = USE_SQL_DATES;
		else if (strcasecmp(DBDate, "POSTGRES") == 0)
			DateStyle = USE_POSTGRES_DATES;
		else if (strcasecmp(DBDate, "GERMAN") == 0)
		{
			DateStyle = USE_GERMAN_DATES;
			EuroDates = TRUE;
		}

		if (strcasecmp(DBDate, "NONEURO") == 0)
			EuroDates = FALSE;
		else if (strcasecmp(DBDate, "EURO") == 0)
			EuroDates = TRUE;
	}

	/*
	 * Read default pg_options from file $DATADIR/pg_options.
	 */
	if(DataDir) {
		read_pg_options(0);
	}

	optind = 1;					/* reset after postmaster usage */

	while ((flag = getopt(argc, argv,
						  "A:B:bCD:d:Eef:iK:Lm:MNo:P:pQS:st:v:x:FW:"))
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

			case 'b':
				/* ----------------
				 *	set BushyPlanFlag to true.
				 * ----------------
				 */
				BushyPlanFlag = 1;
				break;

			case 'B':
				/* ----------------
				 *	specify the size of buffer pool
				 * ----------------
				 */
				NBuffers = atoi(optarg);
				break;

			case 'C':
				/* ----------------
				 *	don't print version string (don't know why this is 'C' --mao)
				 * ----------------
				 */
				flagC = true;
				break;

			case 'D':			/* PGDATA directory */
			        if (!DataDir) {
				    DataDir = optarg;
				    /* must be done after DataDir is defined */
				    read_pg_options(0);
				}
				DataDir = optarg;
				break;

			case 'd':			/* debug level */
				flagQ = false;
				DebugLvl = (short) atoi(optarg);
				if (DebugLvl >= 1)
					Verbose = DebugLvl;
				if (DebugLvl >= 2)
					DebugPrintQuery = true;
				if (DebugLvl >= 3)
					DebugPrintQuery = DebugLvl;
				if (DebugLvl >= 4)
				{
					DebugPrintParse = true;
					DebugPrintPlan = true;
					DebugPrintRewrittenParsetree = true;
				}
				break;

			case 'E':
				/* ----------------
				 *	E - echo the query the user entered
				 * ----------------
				 */
				flagE = true;
				break;

			case 'e':
				/* --------------------------
				 * Use european date formats.
				 * --------------------------
				 */
				flagEu = true;
				break;

			case 'F':
				/* --------------------
				 *	turn off fsync
				 * --------------------
				 */
				fsyncOff = 1;
				break;

			case 'f':
				/* -----------------
				 *	  f - forbid generation of certain plans
				 * -----------------
				 */
				switch (optarg[0])
				{
					case 's':	/* seqscan */
						_enable_seqscan_ = false;
						break;
					case 'i':	/* indexscan */
						_enable_indexscan_ = false;
						break;
					case 'n':	/* nestloop */
						_enable_nestloop_ = false;
						break;
					case 'm':	/* mergejoin */
						_enable_mergejoin_ = false;
						break;
					case 'h':	/* hashjoin */
						_enable_hashjoin_ = false;
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
				lockingOff = 1;
				break;

			case 'm':
				/* Multiplexed backends are no longer supported. */
				break;
			case 'M':
				exit(PostmasterMain(argc, argv));
				break;
			case 'N':
				/* ----------------
				 *	N - Don't use newline as a query delimiter
				 * ----------------
				 */
				UseNewLine = 0;
				break;

			case 'o':
				/* ----------------
				 *	o - send output (stdout and stderr) to the given file
				 * ----------------
				 */
				StrNCpy(OutputFileName, optarg, MAXPGPATH);
				break;

			case 'p':			/* started by postmaster */
				/* ----------------
				 *	p - special flag passed if backend was forked
				 *		by a postmaster.
				 * ----------------
				 */
				IsUnderPostmaster = true;
				break;

			case 'P':
				/* ----------------
				 *	P - Use the passed file descriptor number as the port
				 *	  on which to communicate with the user.  This is ONLY
				 *	  useful for debugging when fired up by the postmaster.
				 * ----------------
				 */
				Portfd = atoi(optarg);
				break;

			case 'Q':
				/* ----------------
				 *	Q - set Quiet mode (reduce debugging output)
				 * ----------------
				 */
				flagQ = true;
				Verbose = 0;
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
				parse_options(optarg);
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
#if 0							/* planner/xfunc.h */

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
		}

	/* ----------------
	 *	get user name and pathname and check command line validity
	 * ----------------
	 */
	SetPgUserName();
	userName = GetPgUserName();

#ifdef CYR_RECODE
	SetCharSet();
#endif

	if (FindExec(pg_pathname, argv[0], "postgres") < 0)
		elog(FATAL, "%s: could not locate executable, bailing out...",
			 argv[0]);

	if (errs || argc - optind > 1)
	{
		usage(argv[0]);
		proc_exit(1);
	}
	else if (argc - optind == 1)
		DBName = argv[optind];
	else if ((DBName = userName) == NULL)
	{
		fprintf(stderr, "%s: USER undefined and no database specified\n",
				argv[0]);
		proc_exit(1);
	}

	if (ShowStats &&
		(ShowParserStats || ShowPlannerStats || ShowExecutorStats))
	{
		fprintf(stderr, "-s can not be used together with -t.\n");
		proc_exit(1);
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

	Noversion = flagC;
	EchoQuery = flagE;
	EuroDates = flagEu;

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
	}

	/* ----------------
	 *	set process params for ps
	 * ----------------
	 */
	if (IsUnderPostmaster)
	{
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
		if (Verbose == 1)
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
			TPRINTF(TRACE_VERBOSE, "\tVerbose      = %d", Verbose);
			TPRINTF(TRACE_VERBOSE, "\tNoversion    = %c", Noversion ? 't' : 'f');
			TPRINTF(TRACE_VERBOSE, "\ttimings      = %c", ShowStats ? 't' : 'f');
			TPRINTF(TRACE_VERBOSE, "\tdates        = %s",
					EuroDates ? "European" : "Normal");
			TPRINTF(TRACE_VERBOSE, "\tbufsize      = %d", NBuffers);
			TPRINTF(TRACE_VERBOSE, "\tsortmem      = %d", SortMem);
			TPRINTF(TRACE_VERBOSE, "\tquery echo   = %c", EchoQuery ? 't' : 'f');
		}
	}

	/* ----------------
	 *	initialize portal file descriptors
	 * ----------------
	 */
	if (IsUnderPostmaster)
	{
		if (Portfd < 0)
		{
			fprintf(stderr,
					"Postmaster flag set: no port number specified, use /dev/null\n");
#ifndef __CYGWIN32__
			Portfd = open(NULL_DEV, O_RDWR, 0666);
#else
			Portfd = open(NULL_DEV, O_RDWR | O_BINARY, 0666);
#endif
		}
		pq_init(Portfd);
		whereToSendOutput = Remote;
	}
	else
		whereToSendOutput = Debug;

	SetProcessingMode(InitProcessing);

	/* initialize */
	if (Verbose)
		TPRINTF(TRACE_VERBOSE, "InitPostgres");

	InitPostgres(DBName);

#ifdef MULTIBYTE
	/* set default client encoding */
	if (Verbose)
		puts("\treset_client_encoding()..");
	reset_client_encoding();
	if (Verbose)
		puts("\treset_client_encoding() done.");
#endif

	/* ----------------
	 * if stable main memory is assumed (-S(old) flag is set), it is necessary
	 * to flush all dirty shared buffers before exit
	 * plai 8/7/90
	 * this used to be done further down, causing an additional entry in
	 * the shmem exit list for every error :-( ... tgl 10/1/98
	 * ----------------
	 */
	if (!TransactionFlushEnabled())
		on_shmem_exit(FlushBufferPool, NULL);

	on_shmem_exit(remove_all_temp_relations, NULL);

	/* ----------------
	 *	Set up handler for cancel-request signal, and
	 *	send this backend's cancellation info to the frontend.
	 *	This should not be done until we are sure startup is successful.
	 * ----------------
	 */

	pqsignal(SIGHUP, read_pg_options);	/* update pg_options from file */
	pqsignal(SIGINT, QueryCancelHandler);		/* cancel current query */
	pqsignal(SIGQUIT, handle_warn);		/* handle error */
	pqsignal(SIGTERM, die);
	pqsignal(SIGPIPE, SIG_IGN);	/* ignore failure to write to frontend */
	/* Note: if frontend closes connection, we will notice it and exit cleanly
	 * when control next returns to outer loop.  This seems safer than forcing
	 * exit in the midst of output during who-knows-what operation...
	 */
	pqsignal(SIGUSR1, quickdie);
	pqsignal(SIGUSR2, Async_NotifyHandler);		/* flush also sinval cache */
	pqsignal(SIGCHLD, SIG_IGN); /* ignored, sent by LockOwners */
	pqsignal(SIGFPE, FloatExceptionHandler);

	if (whereToSendOutput == Remote &&
		PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		pq_putnchar("K", 1);
		pq_putint((int32) MyProcPid, sizeof(int32));
		pq_putint((int32) MyCancelKey, sizeof(int32));
		/* Need not flush since ReadyForQuery will do it. */
	}

	if (!IsUnderPostmaster)
	{
		puts("\nPOSTGRES backend interactive interface ");
		puts("$Revision: 1.99 $ $Date: 1999/02/13 23:18:45 $\n");
	}

	/* ----------------
	 *	POSTGRES main processing loop begins here
	 *
	 *	if an exception is encountered, processing resumes here
	 *	so we abort the current transaction and start a new one.
	 *	This must be done after we initialize the slave backends
	 *	so that the slaves signal the master to abort the transaction
	 *	rather than calling AbortCurrentTransaction() themselves.
	 *
	 *	Note:  elog(ERROR) causes a kill(getpid(),SIGQUIT) to occur
	 *		   sending us back here.
	 * ----------------
	 */

	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		InError = true;

		time(&tim);

		if (Verbose)
			TPRINTF(TRACE_VERBOSE, "AbortCurrentTransaction");

		AbortCurrentTransaction();
	}

	InError = false;

	/*
	 * Non-error queries loop here.
	 */

	for (;;)
	{
		PS_SET_STATUS("idle");

		/* ----------------
		 *	 (1) tell the frontend we're ready for a new query.
		 *
		 *   Note: this includes fflush()'ing the last of the prior output.
		 * ----------------
		 */
		ReadyForQuery(whereToSendOutput);

		/* ----------------
		 *	 (2) deal with pending asynchronous NOTIFY from other backends,
		 *   and enable async.c's signal handler to execute NOTIFY directly.
		 * ----------------
		 */
		QueryCancel = false;	/* forget any earlier CANCEL signal */

		EnableNotifyInterrupt();

		/* ----------------
		 *	 (3) read a command.
		 * ----------------
		 */
		MemSet(parser_input, 0, MAX_PARSE_BUFFER);

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

				HandleFunctionRequest();
				break;

				/* ----------------
				 *	'Q' indicates a user query
				 * ----------------
				 */
			case 'Q':
				if (strspn(parser_input, " \t\n") == strlen(parser_input))
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

					pg_exec_query(parser_input);

					if (ShowStats)
						ShowUsage();
				}
				break;

				/* ----------------
				 *	'X' means that the frontend is closing down the socket
				 * ----------------
				 */
			case 'X':
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
#else							/* HAVE_GETRUSAGE */
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
