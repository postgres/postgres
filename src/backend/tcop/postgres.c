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
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/postgres.c,v 1.179 2000/10/07 04:00:41 tgl Exp $
 *
 * NOTES
 *	  this is the "main" module of the postgres backend and
 *	  hence the main module of the "traffic cop".
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
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
#include "parser/analyze.h"
#include "parser/parse.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/fastpath.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "storage/proc.h"
#include "utils/exc.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/temprel.h"
#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif


/* ----------------
 *		global variables
 * ----------------
 */

/*
 * XXX For ps display. That stuff needs to be cleaned up.
 */
bool HostnameLookup;
bool ShowPortNumber;

bool Log_connections = false;

CommandDest whereToSendOutput = Debug;


extern void StartupXLOG(void);
extern void ShutdownXLOG(void);

extern void HandleDeadLock(SIGNAL_ARGS);

extern char XLogDir[];
extern char ControlFilePath[];

static bool	dontExecute = false;

/* note: these declarations had better match tcopprot.h */
DLLIMPORT sigjmp_buf Warn_restart;

bool		Warn_restart_ready = false;
bool		InError = false;
bool		ExitAfterAbort = false;

static bool EchoQuery = false;	/* default don't echo */
char		pg_pathname[MAXPGPATH];
FILE	   *StatFp = NULL;

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
static List *pg_parse_query(char *query_string, Oid *typev, int nargs);
static List *pg_analyze_and_rewrite(Node *parsetree);
static void start_xact_command(void);
static void finish_xact_command(void);
static void SigHupHandler(SIGNAL_ARGS);
static void FloatExceptionHandler(SIGNAL_ARGS);
static void quickdie(SIGNAL_ARGS);

/*
 * Flag to mark SIGHUP. Whenever the main loop comes around it
 * will reread the configuration file. (Better than doing the
 * reading in the signal handler, ey?)
 */
static volatile bool got_SIGHUP = false;


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
			return EOF;

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
	int			result;

	if (IsUnderPostmaster)
		result = SocketBackend(inBuf);
	else
		result = InteractiveBackend(inBuf);
	return result;
}


/*
 * Parse a query string and pass it through the rewriter.
 *
 * A list of Query nodes is returned, since the string might contain
 * multiple queries and/or the rewriter might expand one query to several.
 *
 * NOTE: this routine is no longer used for processing interactive queries,
 * but it is still needed for parsing of SQL function bodies.
 */
List *
pg_parse_and_rewrite(char *query_string,	/* string to execute */
					 Oid *typev,			/* parameter types */
					 int nargs)				/* number of parameters */
{
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	List	   *list_item;

	/* ----------------
	 *	(1) parse the request string into a list of raw parse trees.
	 * ----------------
	 */
	raw_parsetree_list = pg_parse_query(query_string, typev, nargs);

	/* ----------------
	 *	(2) Do parse analysis and rule rewrite.
	 * ----------------
	 */
	querytree_list = NIL;
	foreach(list_item, raw_parsetree_list)
	{
		Node   *parsetree = (Node *) lfirst(list_item);

		querytree_list = nconc(querytree_list,
							   pg_analyze_and_rewrite(parsetree));
	}

	return querytree_list;
}

/*
 * Do raw parsing (only).
 *
 * A list of parsetrees is returned, since there might be multiple
 * commands in the given string.
 *
 * NOTE: for interactive queries, it is important to keep this routine
 * separate from the analysis & rewrite stages.  Analysis and rewriting
 * cannot be done in an aborted transaction, since they require access to
 * database tables.  So, we rely on the raw parser to determine whether
 * we've seen a COMMIT or ABORT command; when we are in abort state, other
 * commands are not processed any further than the raw parse stage.
 */
static List *
pg_parse_query(char *query_string, Oid *typev, int nargs)
{
	List	   *raw_parsetree_list;

	if (Debug_print_query)
		elog(DEBUG, "query: %s", query_string);

	if (Show_parser_stats)
		ResetUsage();

	raw_parsetree_list = parser(query_string, typev, nargs);

	if (Show_parser_stats)
	{
		fprintf(StatFp, "PARSER STATISTICS\n");
		ShowUsage();
	}

	return raw_parsetree_list;
}

/*
 * Given a raw parsetree (gram.y output), perform parse analysis and
 * rule rewriting.
 *
 * A list of Query nodes is returned, since either the analyzer or the
 * rewriter might expand one query to several.
 *
 * NOTE: for reasons mentioned above, this must be separate from raw parsing.
 */
static List *
pg_analyze_and_rewrite(Node *parsetree)
{
	List	   *querytree_list;
	List	   *list_item;
	Query	   *querytree;
	List	   *new_list;

	/* ----------------
	 *	(1) Perform parse analysis.
	 * ----------------
	 */
	if (Show_parser_stats)
		ResetUsage();

	querytree_list = parse_analyze(parsetree, NULL);

	if (Show_parser_stats)
	{
		fprintf(StatFp, "PARSE ANALYSIS STATISTICS\n");
		ShowUsage();
		ResetUsage();
	}

	/* ----------------
	 *	(2) Rewrite the queries, as necessary
	 *
	 *	rewritten queries are collected in new_list.  Note there may be
	 *	more or fewer than in the original list.
	 * ----------------
	 */
	new_list = NIL;
	foreach(list_item, querytree_list)
	{
		querytree = (Query *) lfirst(list_item);

		if (Debug_print_parse)
		{
			if (Debug_pretty_print)
			{
				elog(DEBUG, "parse tree:");
				nodeDisplay(querytree);
			}
			else
				elog(DEBUG, "parse tree: %s", nodeToString(querytree));
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

	if (Show_parser_stats)
	{
		fprintf(StatFp, "REWRITER STATISTICS\n");
		ShowUsage();
	}

#ifdef COPY_PARSE_PLAN_TREES
	/* Optional debugging check: pass querytree output through copyObject() */
	new_list = (List *) copyObject(querytree_list);
	/* This checks both copyObject() and the equal() routines... */
	if (! equal(new_list, querytree_list))
		elog(NOTICE, "pg_analyze_and_rewrite: copyObject failed on parse tree");
	else
		querytree_list = new_list;
#endif

	if (Debug_print_rewritten)
	{
		if (Debug_pretty_print)
		{
			elog(DEBUG, "rewritten parse tree:");
			foreach(list_item, querytree_list)
			{
				querytree = (Query *) lfirst(list_item);
				nodeDisplay(querytree);
				printf("\n");
			}
		}
		else
		{
			elog(DEBUG, "rewritten parse tree:");
			foreach(list_item, querytree_list)
			{
				querytree = (Query *) lfirst(list_item);
				elog(DEBUG, "%s", nodeToString(querytree));
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

	if (Show_planner_stats)
		ResetUsage();

	/* call that optimizer */
	plan = planner(querytree);

	if (Show_planner_stats)
	{
		fprintf(stderr, "PLANNER STATISTICS\n");
		ShowUsage();
	}

#ifdef COPY_PARSE_PLAN_TREES
	/* Optional debugging check: pass plan output through copyObject() */
	{
		Plan   *new_plan = (Plan *) copyObject(plan);

		/* equal() currently does not have routines to compare Plan nodes,
		 * so don't try to test equality here.  Perhaps fix someday?
		 */
#ifdef NOT_USED
		/* This checks both copyObject() and the equal() routines... */
		if (! equal(new_plan, plan))
			elog(NOTICE, "pg_plan_query: copyObject failed on plan tree");
		else
#endif
			plan = new_plan;
	}
#endif

	/* ----------------
	 *	Print plan if debugging.
	 * ----------------
	 */
	if (Debug_print_plan)
	{
		if (Debug_pretty_print)
		{
			elog(DEBUG, "plan:");
			nodeDisplay(plan);
		}
		else
			elog(DEBUG, "plan: %s", nodeToString(plan));
	}

	return plan;
}


/* ----------------------------------------------------------------
 *		pg_exec_query_string()
 *
 *		Takes a querystring, runs the parser/utilities or
 *		parser/planner/executor over it as necessary.
 *
 * Assumptions:
 *
 * At call, we are not inside a transaction command.
 *
 * The CurrentMemoryContext after starting a transaction command must be
 * appropriate for execution of individual queries (typically this will be
 * TransactionCommandContext).  Note that this routine resets that context
 * after each individual query, so don't store anything there that
 * must outlive the call!
 *
 * parse_context references a context suitable for holding the
 * parse/rewrite trees (typically this will be QueryContext).
 * This context *must* be longer-lived than the transaction context!
 * In fact, if the query string might contain BEGIN/COMMIT commands,
 * parse_context had better outlive TopTransactionContext!
 *
 * We could have hard-wired knowledge about QueryContext and
 * TransactionCommandContext into this routine, but it seems better
 * not to, in case callers from outside this module need to use some
 * other contexts.
 *
 * ----------------------------------------------------------------
 */

void
pg_exec_query_string(char *query_string,	/* string to execute */
					 CommandDest dest,		/* where results should go */
					 MemoryContext parse_context) /* context for parsetrees */
{
	bool		xact_started;
	MemoryContext oldcontext;
	List	   *parsetree_list,
			   *parsetree_item;

	/*
	 * Start up a transaction command.  All queries generated by the
	 * query_string will be in this same command block, *unless* we find
	 * a BEGIN/COMMIT/ABORT statement; we have to force a new xact command
	 * after one of those, else bad things will happen in xact.c.
	 * (Note that this will possibly change current memory context.)
	 */
	start_xact_command();
	xact_started = true;

	/*
	 * parse_context *must* be different from the execution memory context,
	 * else the context reset at the bottom of the loop will destroy the
	 * parsetree list.  (We really ought to check that parse_context isn't a
	 * child of CurrentMemoryContext either, but that would take more cycles
	 * than it's likely to be worth.)
	 */
	Assert(parse_context != CurrentMemoryContext);

	/*
	 * Switch to appropriate context for constructing parsetrees.
	 */
	oldcontext = MemoryContextSwitchTo(parse_context);

	/*
	 * Do basic parsing of the query or queries (this should be safe
	 * even if we are in aborted transaction state!)
	 */
	parsetree_list = pg_parse_query(query_string, NULL, 0);

	/*
	 * Switch back to execution context to enter the loop.
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Run through the parsetree(s) and process each one.
	 */
	foreach(parsetree_item, parsetree_list)
	{
		Node   *parsetree = (Node *) lfirst(parsetree_item);
		bool	isTransactionStmt;
		List   *querytree_list,
			   *querytree_item;

		/* Transaction control statements need some special handling */
		isTransactionStmt = IsA(parsetree, TransactionStmt);

		/*
		 * If we are in an aborted transaction, ignore all commands except
		 * COMMIT/ABORT.  It is important that this test occur before we
		 * try to do parse analysis, rewrite, or planning, since all those
		 * phases try to do database accesses, which may fail in abort state.
		 * (It might be safe to allow some additional utility commands in
		 * this state, but not many...)
		 */
		if (IsAbortedTransactionBlockState())
		{
			bool	allowit = false;

			if (isTransactionStmt)
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->command)
				{
					case COMMIT:
					case ROLLBACK:
						allowit = true;
						break;
					default:
						break;
				}
			}

			if (! allowit)
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
		}

		/* Make sure we are in a transaction command */
		if (! xact_started)
		{
			start_xact_command();
			xact_started = true;
		}

		/* If we got a cancel signal in parsing or prior command, quit */
		if (QueryCancel)
			CancelQuery();

		/*
		 * OK to analyze and rewrite this query.
		 *
		 * Switch to appropriate context for constructing querytrees
		 * (again, these must outlive the execution context).
		 */
		oldcontext = MemoryContextSwitchTo(parse_context);

		querytree_list = pg_analyze_and_rewrite(parsetree);

		/*
		 * Switch back to execution context for planning and execution.
		 */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * Inner loop handles the individual queries generated from a
		 * single parsetree by analysis and rewrite.
		 */
		foreach(querytree_item, querytree_list)
		{
			Query	   *querytree = (Query *) lfirst(querytree_item);

			/* Make sure we are in a transaction command */
			if (! xact_started)
			{
				start_xact_command();
				xact_started = true;
			}

			/* If we got a cancel signal in analysis or prior command, quit */
			if (QueryCancel)
				CancelQuery();

			if (querytree->commandType == CMD_UTILITY)
			{
				/* ----------------
				 *	 process utility functions (create, destroy, etc..)
				 * ----------------
				 */
				if (Debug_print_query)
					elog(DEBUG, "ProcessUtility: %s", query_string);
				else if (DebugLvl > 1)
					elog(DEBUG, "ProcessUtility");

				ProcessUtility(querytree->utilityStmt, dest);
			}
			else
			{
				/* ----------------
				 *	 process a plannable query.
				 * ----------------
				 */
				Plan	   *plan;

				plan = pg_plan_query(querytree);

				/* if we got a cancel signal whilst planning, quit */
				if (QueryCancel)
					CancelQuery();

				/* Initialize snapshot state for query */
				SetQuerySnapshot();

				/*
				 * execute the plan
				 */
				if (Show_executor_stats)
					ResetUsage();

				if (dontExecute)
				{
					/* don't execute it, just show the query plan */
					print_plan(plan, querytree);
				}
				else
				{
					if (DebugLvl > 1)
						elog(DEBUG, "ProcessQuery");
					ProcessQuery(querytree, plan, dest);
				}

				if (Show_executor_stats)
				{
					fprintf(stderr, "EXECUTOR STATISTICS\n");
					ShowUsage();
				}
			}

			/*
			 * In a query block, we want to increment the command counter
			 * between queries so that the effects of early queries are
			 * visible to subsequent ones.  In particular we'd better
			 * do so before checking constraints.
			 */
			if (!isTransactionStmt)
				CommandCounterIncrement();

			/*
			 * Clear the execution context to recover temporary
			 * memory used by the query.  NOTE: if query string contains
			 * BEGIN/COMMIT transaction commands, execution context may
			 * now be different from what we were originally passed;
			 * so be careful to clear current context not "oldcontext".
			 */
			Assert(parse_context != CurrentMemoryContext);

			MemoryContextResetAndDeleteChildren(CurrentMemoryContext);

			/*
			 * If this was a transaction control statement, commit it
			 * and arrange to start a new xact command for the next
			 * command (if any).
			 */
			if (isTransactionStmt)
			{
				finish_xact_command();
				xact_started = false;
			}

		} /* end loop over queries generated from a parsetree */
	} /* end loop over parsetrees */

	/*
	 * Close down transaction statement, if one is open.
	 */
	if (xact_started)
		finish_xact_command();
}

/*
 * Convenience routines for starting/committing a single command.
 */
static void
start_xact_command(void)
{
	if (DebugLvl >= 1)
		elog(DEBUG, "StartTransactionCommand");
	StartTransactionCommand();
}

static void
finish_xact_command(void)
{
	/* Invoke IMMEDIATE constraint triggers */
	DeferredTriggerEndQuery();

	/* Now commit the command */
	if (DebugLvl >= 1)
		elog(DEBUG, "CommitTransactionCommand");
	set_ps_display("commit");	/* XXX probably the wrong place to do this */
	CommitTransactionCommand();

#ifdef SHOW_MEMORY_STATS
	/* Print mem stats at each commit for leak tracking */
	if (ShowStats)
		MemoryContextStats(TopMemoryContext);
#endif
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

static void
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
static void
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
SigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
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
	fprintf(stderr, "\t-F \t\tturn fsync off\n");
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
 * PostgresMain
 *     postgres main loop -- all backends, interactive or otherwise start here
 *
 * argc/argv are the command line arguments to be used.  When being forked
 * by the postmaster, these are not the original argv array of the process.
 * real_argc/real_argv point to the original argv array, which is needed by
 * `ps' display on some platforms. username is the (possibly authenticated)
 * PostgreSQL user name to be used for the session.
 * ----------------------------------------------------------------
 */
int
PostgresMain(int argc, char *argv[], int real_argc, char *real_argv[], const char * username)
{
	int			flag;

	const char	   *DBName = NULL;
	bool		secure = true;
	int			errs = 0;

	int			firstchar;
	StringInfo	parser_input;

	char	   *remote_host;
	unsigned short remote_port;

	extern int	optind;
	extern char *optarg;
	extern int	DebugLvl;

	/*
	 * Fire up essential subsystems: error and memory management
	 *
	 * If we are running under the postmaster, this is done already.
	 */
	if (!IsUnderPostmaster)
	{
		EnableExceptionHandling(true);
		MemoryContextInit();
	}

	/*
	 * Set default values for command-line options.
	 */
	Noversion = false;
	EchoQuery = false;

	if (!IsUnderPostmaster)
	{
		ResetAllOptions();
		if (getenv("PGDATA"))
			DataDir = strdup(getenv("PGDATA"));
	}
	StatFp = stderr;

	SetProcessingMode(InitProcessing);

	/* Check for PGDATESTYLE environment variable */
	set_default_datestyle();

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

	while ((flag = getopt(argc, argv,  "A:B:CD:d:Eef:FiLNOPo:p:S:st:v:W:x:-:")) != EOF)
		switch (flag)
		{
			case 'A':
#ifdef USE_ASSERT_CHECKING
				assert_enabled = atoi(optarg);
#else
				fprintf(stderr, "Assert checking is not compiled in\n");
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
					if (DataDir)
						free(DataDir);
					DataDir = strdup(optarg);
				}
				break;

			case 'd':			/* debug level */
				DebugLvl = atoi(optarg);
				if (DebugLvl >= 1);
					Log_connections = true;
				if (DebugLvl >= 2)
					Debug_print_query = true;
				if (DebugLvl >= 3)
					Debug_print_parse = true;
				if (DebugLvl >= 4)
					Debug_print_plan = true;
				if (DebugLvl >= 5)
					Debug_print_rewritten = true;
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
				enableFsync = false;
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
				dontExecute = true;
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
					DBName = strdup(optarg);
					secure = false;		/* subsequent switches are NOT
										 * secure */
				}
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
				Show_query_stats = 1;
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
				switch (optarg[0])
				{
					case 'p':
						if (optarg[1] == 'a')
							Show_parser_stats = 1;
						else if (optarg[1] == 'l')
							Show_planner_stats = 1;
						else
							errs++;
						break;
					case 'e':
						Show_executor_stats = 1;
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

			case '-':
			{
				char *name, *value;

				ParseLongOption(optarg, &name, &value);
				if (!value)
					elog(ERROR, "--%s requires argument", optarg);

				SetConfigOption(name, value, PGC_BACKEND);
				free(name);
				if (value)
					free(value);
				break;
			}

			default:
				/* ----------------
				 *	default: bad command line option
				 * ----------------
				 */
				errs++;
				break;
		}

	if (Show_query_stats &&
		(Show_parser_stats || Show_planner_stats || Show_executor_stats))
	{
		elog(NOTICE, "Query statistics are disabled because parser, planner, or executor statistics are on.");
		Show_query_stats = false;
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
	pqinitmask();

#ifdef HAVE_SIGPROCMASK
	sigdelset(&BlockSig, SIGUSR1);
#else
	BlockSig &= ~(sigmask(SIGUSR1));
#endif

	PG_SETMASK(&BlockSig);		/* block everything except SIGUSR1 */

	pqsignal(SIGHUP, SigHupHandler);	/* set flag to read config file */
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
		else if ((DBName = username) == NULL)
		{
			fprintf(stderr, "%s: user name undefined and no database specified\n",
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
		on_proc_exit(UnlinkPidFile, 0);

		BaseInit();
		snprintf(XLogDir, MAXPGPATH, "%s/pg_xlog", DataDir);
		snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);
		StartupXLOG();
	}

	/*
	 * Set up additional info.
	 */

#ifdef CYR_RECODE
	SetCharSet();
#endif

	/* On some systems our dynloader code needs the executable's pathname */
	if (FindExec(pg_pathname, real_argv[0], "postgres") < 0)
		elog(FATAL, "%s: could not locate executable, bailing out...",
			 real_argv[0]);

	/*
	 * Find remote host name or address.
	 */
	remote_host = NULL;

	if (IsUnderPostmaster)
	{
		if (MyProcPort->raddr.sa.sa_family == AF_INET)
		{
			struct hostent *host_ent;
			char * host_addr;

			remote_port = ntohs(MyProcPort->raddr.in.sin_port);
			host_addr = inet_ntoa(MyProcPort->raddr.in.sin_addr);

			if (HostnameLookup)
			{
				host_ent = gethostbyaddr((char *) &MyProcPort->raddr.in.sin_addr, sizeof(MyProcPort->raddr.in.sin_addr), AF_INET);

				if (host_ent)
				{
					remote_host = palloc(strlen(host_addr) + strlen(host_ent->h_name) + 3);
					sprintf(remote_host, "%s[%s]", host_ent->h_name, host_addr);
				}
			}

			if (remote_host == NULL)
				remote_host = pstrdup(host_addr);

			if (ShowPortNumber)
			{
				char * str = palloc(strlen(remote_host) + 7);
				sprintf(str, "%s:%hu", remote_host, remote_port);
				pfree(remote_host);
				remote_host = str;
			}
		}
		else /* not AF_INET */
			remote_host = "[local]";


		/*
		 * Set process parameters for ps
		 *
		 * WARNING: On some platforms the environment will be moved
		 * around to make room for the ps display string. So any
		 * references to optarg or getenv() from above will be invalid
		 * after this call. Better use strdup or something similar.
		 */
		init_ps_display(real_argc, real_argv, username, DBName, remote_host);
		set_ps_display("startup");
	}

	if (Log_connections)
		elog(DEBUG, "connection: host=%s user=%s database=%s",
			 remote_host, username, DBName);

	/*
	 * general initialization
	 */
	if (DebugLvl > 1)
		elog(DEBUG, "InitPostgres");
	InitPostgres(DBName, username);

#ifdef MULTIBYTE
	/* set default client encoding */
	if (DebugLvl > 1)
		elog(DEBUG, "reset_client_encoding");
	reset_client_encoding();
#endif

	on_shmem_exit(remove_all_temp_relations, 0);

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
		puts("$Revision: 1.179 $ $Date: 2000/10/07 04:00:41 $\n");
	}

	/*
	 * Initialize the deferred trigger manager
	 */
	if (DeferredTriggerInit() != 0)
		goto normalexit;

	SetProcessingMode(NormalProcessing);

	/*
	 * Create the memory context we will use in the main loop.
	 *
	 * QueryContext is reset once per iteration of the main loop,
	 * ie, upon completion of processing of each supplied query string.
	 * It can therefore be used for any data that should live just as
	 * long as the query string --- parse trees, for example.
	 */
	QueryContext = AllocSetContextCreate(TopMemoryContext,
										 "QueryContext",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * POSTGRES main processing loop begins here
	 *
	 * If an exception is encountered, processing resumes here so we abort
	 * the current transaction and start a new one.
	 */

	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		/*
		 * Make sure we are in a valid memory context during recovery.
		 *
		 * We use ErrorContext in hopes that it will have some free space
		 * even if we're otherwise up against it...
		 */
		MemoryContextSwitchTo(ErrorContext);

		if (DebugLvl >= 1)
			elog(DEBUG, "AbortCurrentTransaction");
		AbortCurrentTransaction();

		if (ExitAfterAbort)
			goto errorexit;

		/*
		 * If we recovered successfully, return to normal top-level context
		 * and clear ErrorContext for next time.
		 */
		MemoryContextSwitchTo(TopMemoryContext);
		MemoryContextResetAndDeleteChildren(ErrorContext);
		InError = false;
	}

	Warn_restart_ready = true;	/* we can now handle elog(ERROR) */

	PG_SETMASK(&UnBlockSig);

	/*
	 * Non-error queries loop here.
	 */

	for (;;)
	{
		/*
		 * Release storage left over from prior query cycle, and
		 * create a new query input buffer in the cleared QueryContext.
		 */
		MemoryContextSwitchTo(QueryContext);
		MemoryContextResetAndDeleteChildren(QueryContext);

		parser_input = makeStringInfo();

		/* XXX this could be moved after ReadCommand below to get more
		 * sensical behaviour */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

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
		 *	 (3) read a command (loop blocks here)
		 * ----------------
		 */
		set_ps_display("idle");

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
				 * ----------------
				 */
			case 'F':
				/* start an xact for this function invocation */
				start_xact_command();

				if (HandleFunctionRequest() == EOF)
				{
					/* lost frontend connection during F message input */
					goto normalexit;
				}

				/* commit the function-invocation transaction */
				finish_xact_command();
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
					 *	trying to parse and execute anything; just send
					 *	back a quick NullCommand response.
					 * ----------------
					 */
					if (IsUnderPostmaster)
						NullCommand(Remote);
				}
				else
				{
					/* ----------------
					 *	otherwise, process the input string.
					 *
					 * Note: transaction command start/end is now done
					 * within pg_exec_query_string(), not here.
					 * ----------------
					 */
					if (Show_query_stats)
						ResetUsage();

					pg_exec_query_string(parser_input->data,
										 whereToSendOutput,
										 QueryContext);

					if (Show_query_stats)
					{
						fprintf(StatFp, "QUERY STATISTICS\n");
						ShowUsage();
					}
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
				goto normalexit;

			default:
				elog(ERROR, "unknown frontend message was received");
		}

#ifdef MEMORY_CONTEXT_CHECKING
		/*
		 * Check all memory after each backend loop.  This is a rather
		 * weird place to do it, perhaps.
		 */
		MemoryContextCheck(TopMemoryContext);	
#endif
	}							/* end of input-reading loop */

normalexit:
	ExitAfterAbort = true;		/* ensure we will exit if elog during abort */
	AbortOutOfAnyTransaction();
	if (!IsUnderPostmaster)
		ShutdownXLOG();

errorexit:
	pq_close();
	ProcReleaseLocks();			/* Just to be sure... */
	proc_exit(0);

	return 1;					/* keep compiler quiet */
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
	 * Set output destination if not otherwise set
	 */
	if (StatFp == NULL)
		StatFp = stderr;

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
/* BeOS has rusage but only has some fields, and not these... */
#if defined(HAVE_GETRUSAGE) && !defined(__BEOS__)
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

#ifdef NOT_USED
static int
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
