/*-------------------------------------------------------------------------
 *
 * postgres.c
 *	  POSTGRES C Backend Interface
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tcop/postgres.c,v 1.433 2004/09/26 00:26:25 tgl Exp $
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
#include <fcntl.h>
#include <sys/socket.h>
#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "access/printtup.h"
#include "access/xlog.h"
#include "catalog/pg_type.h"
#include "commands/async.h"
#include "commands/prepare.h"
#include "commands/trigger.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/print.h"
#include "optimizer/cost.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "tcop/fastpath.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "mb/pg_wchar.h"

#include "pgstat.h"

extern int	optind;
extern char *optarg;

/* ----------------
 *		global variables
 * ----------------
 */
const char *debug_query_string; /* for pgmonitor and
								 * log_min_error_statement */

/* Note: whereToSendOutput is initialized for the bootstrap/standalone case */
CommandDest whereToSendOutput = Debug;

/* flag for logging end of session */
bool		Log_disconnections = false;

LogStmtLevel log_statement = LOGSTMT_NONE;

/* GUC variable for maximum stack depth (measured in kilobytes) */
int			max_stack_depth = 2048;


/* ----------------
 *		private variables
 * ----------------
 */

/* max_stack_depth converted to bytes for speed of checking */
static int	max_stack_depth_bytes = 2048 * 1024;

/* stack base pointer (initialized by PostgresMain) */
static char *stack_base_ptr = NULL;


/*
 * Flag to mark SIGHUP. Whenever the main loop comes around it
 * will reread the configuration file. (Better than doing the
 * reading in the signal handler, ey?)
 */
static volatile sig_atomic_t got_SIGHUP = false;

/*
 * Flag to keep track of whether we have started a transaction.
 * For extended query protocol this has to be remembered across messages.
 */
static bool xact_started = false;

/*
 * Flags to implement skip-till-Sync-after-error behavior for messages of
 * the extended query protocol.
 */
static bool doing_extended_query_message = false;
static bool ignore_till_sync = false;

/*
 * If an unnamed prepared statement exists, it's stored here.
 * We keep it separate from the hashtable kept by commands/prepare.c
 * in order to reduce overhead for short-lived queries.
 */
static MemoryContext unnamed_stmt_context = NULL;
static PreparedStatement *unnamed_stmt_pstmt = NULL;


static bool EchoQuery = false;	/* default don't echo */

/*
 * people who want to use EOF should #define DONTUSENEWLINE in
 * tcop/tcopdebug.h
 */
#ifndef TCOP_DONTUSENEWLINE
static int	UseNewLine = 1;		/* Use newlines query delimiters (the
								 * default) */

#else
static int	UseNewLine = 0;		/* Use EOF as query delimiters */
#endif   /* TCOP_DONTUSENEWLINE */


/* ----------------------------------------------------------------
 *		decls for routines only used in this file
 * ----------------------------------------------------------------
 */
static int	InteractiveBackend(StringInfo inBuf);
static int	SocketBackend(StringInfo inBuf);
static int	ReadCommand(StringInfo inBuf);
static void start_xact_command(void);
static void finish_xact_command(void);
static void SigHupHandler(SIGNAL_ARGS);
static void FloatExceptionHandler(SIGNAL_ARGS);
static void log_disconnections(int code, Datum arg);


/* ----------------------------------------------------------------
 *		routines to obtain user input
 * ----------------------------------------------------------------
 */

/* ----------------
 *	InteractiveBackend() is called for user interactive connections
 *
 *	the string entered by the user is placed in its parameter inBuf,
 *	and we act like a Q message was received.
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

	/*
	 * display a prompt and obtain input from the user
	 */
	printf("backend> ");
	fflush(stdout);

	/* Reset inBuf to empty */
	inBuf->len = 0;
	inBuf->data[0] = '\0';
	inBuf->cursor = 0;

	for (;;)
	{
		if (UseNewLine)
		{
			/*
			 * if we are using \n as a delimiter, then read characters
			 * until the \n.
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
			/*
			 * otherwise read characters until EOF.
			 */
			while ((c = getc(stdin)) != EOF)
				appendStringInfoChar(inBuf, (char) c);

			if (inBuf->len == 0)
				end = true;
		}

		if (end)
			return EOF;

		/*
		 * otherwise we have a user query so process it.
		 */
		break;
	}

	/* Add '\0' to make it look the same as message case. */
	appendStringInfoChar(inBuf, (char) '\0');

	/*
	 * if the query echo flag was given, print the query..
	 */
	if (EchoQuery)
		printf("statement: %s\n", inBuf->data);
	fflush(stdout);

	return 'Q';
}

/* ----------------
 *	SocketBackend()		Is called for frontend-backend connections
 *
 *	Returns the message type code, and loads message body data into inBuf.
 *
 *	EOF is returned if the connection is lost.
 * ----------------
 */
static int
SocketBackend(StringInfo inBuf)
{
	int			qtype;

	/*
	 * Get message type code from the frontend.
	 */
	qtype = pq_getbyte();

	if (qtype == EOF)			/* frontend disconnected */
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("unexpected EOF on client connection")));
		return qtype;
	}

	/*
	 * Validate message type code before trying to read body; if we have
	 * lost sync, better to say "command unknown" than to run out of
	 * memory because we used garbage as a length word.
	 *
	 * This also gives us a place to set the doing_extended_query_message
	 * flag as soon as possible.
	 */
	switch (qtype)
	{
		case 'Q':				/* simple query */
			doing_extended_query_message = false;
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
			{
				/* old style without length word; convert */
				if (pq_getstring(inBuf))
				{
					ereport(COMMERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unexpected EOF on client connection")));
					return EOF;
				}
			}
			break;

		case 'F':				/* fastpath function call */
			/* we let fastpath.c cope with old-style input of this */
			doing_extended_query_message = false;
			break;

		case 'X':				/* terminate */
			doing_extended_query_message = false;
			ignore_till_sync = false;
			break;

		case 'B':				/* bind */
		case 'C':				/* close */
		case 'D':				/* describe */
		case 'E':				/* execute */
		case 'H':				/* flush */
		case 'P':				/* parse */
			doing_extended_query_message = true;
			/* these are only legal in protocol 3 */
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid frontend message type %d", qtype)));
			break;

		case 'S':				/* sync */
			/* stop any active skip-till-Sync */
			ignore_till_sync = false;
			/* mark not-extended, so that a new error doesn't begin skip */
			doing_extended_query_message = false;
			/* only legal in protocol 3 */
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid frontend message type %d", qtype)));
			break;

		case 'd':				/* copy data */
		case 'c':				/* copy done */
		case 'f':				/* copy fail */
			doing_extended_query_message = false;
			/* these are only legal in protocol 3 */
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid frontend message type %d", qtype)));
			break;

		default:

			/*
			 * Otherwise we got garbage from the frontend.	We treat this
			 * as fatal because we have probably lost message boundary
			 * sync, and there's no good way to recover.
			 */
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid frontend message type %d", qtype)));
			break;
	}

	/*
	 * In protocol version 3, all frontend messages have a length word
	 * next after the type code; we can read the message contents
	 * independently of the type.
	 */
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		if (pq_getmessage(inBuf, 0))
			return EOF;			/* suitable message already logged */
	}

	return qtype;
}

/* ----------------
 *		ReadCommand reads a command from either the frontend or
 *		standard input, places it in inBuf, and returns the
 *		message type code (first byte of the message).
 *		EOF is returned if end of file.
 * ----------------
 */
static int
ReadCommand(StringInfo inBuf)
{
	int			result;

	if (whereToSendOutput == Remote)
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
pg_parse_and_rewrite(const char *query_string,	/* string to execute */
					 Oid *paramTypes,	/* parameter types */
					 int numParams)		/* number of parameters */
{
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	ListCell   *list_item;

	/*
	 * (1) parse the request string into a list of raw parse trees.
	 */
	raw_parsetree_list = pg_parse_query(query_string);

	/*
	 * (2) Do parse analysis and rule rewrite.
	 */
	querytree_list = NIL;
	foreach(list_item, raw_parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(list_item);

		querytree_list = list_concat(querytree_list,
									 pg_analyze_and_rewrite(parsetree,
															paramTypes,
															numParams));
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
List *
pg_parse_query(const char *query_string)
{
	List	   *raw_parsetree_list;
	ListCell   *parsetree_item;

	if (log_statement == LOGSTMT_ALL)
		ereport(LOG,
				(errmsg("statement: %s", query_string)));

	if (log_parser_stats)
		ResetUsage();

	raw_parsetree_list = raw_parser(query_string);

	/* do log_statement tests for mod and ddl */
	if (log_statement == LOGSTMT_MOD ||
		log_statement == LOGSTMT_DDL)
	{
		foreach(parsetree_item, raw_parsetree_list)
		{
			Node	   *parsetree = (Node *) lfirst(parsetree_item);
			const char *commandTag;

			if (IsA(parsetree, ExplainStmt) &&
				((ExplainStmt *) parsetree)->analyze)
				parsetree = (Node *) (((ExplainStmt *) parsetree)->query);

			if (IsA(parsetree, PrepareStmt))
				parsetree = (Node *) (((PrepareStmt *) parsetree)->query);

			if (IsA(parsetree, SelectStmt))
				continue;		/* optimization for frequent command */

			if (log_statement == LOGSTMT_MOD &&
				(IsA(parsetree, InsertStmt) ||
				 IsA(parsetree, UpdateStmt) ||
				 IsA(parsetree, DeleteStmt) ||
				 IsA(parsetree, TruncateStmt) ||
				 (IsA(parsetree, CopyStmt) &&
				  ((CopyStmt *) parsetree)->is_from)))	/* COPY FROM */
			{
				ereport(LOG,
						(errmsg("statement: %s", query_string)));
				break;
			}
			commandTag = CreateCommandTag(parsetree);
			if (strncmp(commandTag, "CREATE ", strlen("CREATE ")) == 0 ||
				strncmp(commandTag, "ALTER ", strlen("ALTER ")) == 0 ||
				strncmp(commandTag, "DROP ", strlen("DROP ")) == 0 ||
				IsA(parsetree, GrantStmt) ||	/* GRANT or REVOKE */
				IsA(parsetree, CommentStmt))
			{
				ereport(LOG,
						(errmsg("statement: %s", query_string)));
				break;
			}
		}
	}

	if (log_parser_stats)
		ShowUsage("PARSER STATISTICS");

	return raw_parsetree_list;
}

/*
 * Given a raw parsetree (gram.y output), and optionally information about
 * types of parameter symbols ($n), perform parse analysis and rule rewriting.
 *
 * A list of Query nodes is returned, since either the analyzer or the
 * rewriter might expand one query to several.
 *
 * NOTE: for reasons mentioned above, this must be separate from raw parsing.
 */
List *
pg_analyze_and_rewrite(Node *parsetree, Oid *paramTypes, int numParams)
{
	List	   *querytree_list;

	/*
	 * (1) Perform parse analysis.
	 */
	if (log_parser_stats)
		ResetUsage();

	querytree_list = parse_analyze(parsetree, paramTypes, numParams);

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) Rewrite the queries, as necessary
	 */
	querytree_list = pg_rewrite_queries(querytree_list);

	return querytree_list;
}

/*
 * Perform rewriting of a list of queries produced by parse analysis.
 */
List *
pg_rewrite_queries(List *querytree_list)
{
	List	   *new_list = NIL;
	ListCell   *list_item;

	if (log_parser_stats)
		ResetUsage();

	/*
	 * rewritten queries are collected in new_list.  Note there may be
	 * more or fewer than in the original list.
	 */
	foreach(list_item, querytree_list)
	{
		Query	   *querytree = (Query *) lfirst(list_item);

		if (Debug_print_parse)
			elog_node_display(DEBUG1, "parse tree", querytree,
							  Debug_pretty_print);

		if (querytree->commandType == CMD_UTILITY)
		{
			/* don't rewrite utilities, just dump 'em into new_list */
			new_list = lappend(new_list, querytree);
		}
		else
		{
			/* rewrite regular queries */
			List	   *rewritten = QueryRewrite(querytree);

			new_list = list_concat(new_list, rewritten);
		}
	}

	querytree_list = new_list;

	if (log_parser_stats)
		ShowUsage("REWRITER STATISTICS");

#ifdef COPY_PARSE_PLAN_TREES

	/*
	 * Optional debugging check: pass querytree output through
	 * copyObject()
	 */
	new_list = (List *) copyObject(querytree_list);
	/* This checks both copyObject() and the equal() routines... */
	if (!equal(new_list, querytree_list))
		elog(WARNING, "copyObject() failed to produce an equal parse tree");
	else
		querytree_list = new_list;
#endif

	if (Debug_print_rewritten)
		elog_node_display(DEBUG1, "rewritten parse tree", querytree_list,
						  Debug_pretty_print);

	return querytree_list;
}


/* Generate a plan for a single already-rewritten query. */
Plan *
pg_plan_query(Query *querytree, ParamListInfo boundParams)
{
	Plan	   *plan;

	/* Utility commands have no plans. */
	if (querytree->commandType == CMD_UTILITY)
		return NULL;

	if (log_planner_stats)
		ResetUsage();

	/* call the optimizer */
	plan = planner(querytree, false, 0, boundParams);

	if (log_planner_stats)
		ShowUsage("PLANNER STATISTICS");

#ifdef COPY_PARSE_PLAN_TREES
	/* Optional debugging check: pass plan output through copyObject() */
	{
		Plan	   *new_plan = (Plan *) copyObject(plan);

		/*
		 * equal() currently does not have routines to compare Plan nodes,
		 * so don't try to test equality here.  Perhaps fix someday?
		 */
#ifdef NOT_USED
		/* This checks both copyObject() and the equal() routines... */
		if (!equal(new_plan, plan))
			elog(WARNING, "copyObject() failed to produce an equal plan tree");
		else
#endif
			plan = new_plan;
	}
#endif

	/*
	 * Print plan if debugging.
	 */
	if (Debug_print_plan)
		elog_node_display(DEBUG1, "plan", plan, Debug_pretty_print);

	return plan;
}

/*
 * Generate plans for a list of already-rewritten queries.
 *
 * If needSnapshot is TRUE, we haven't yet set a snapshot for the current
 * query.  A snapshot must be set before invoking the planner, since it
 * might try to evaluate user-defined functions.  But we must not set a
 * snapshot if the list contains only utility statements, because some
 * utility statements depend on not having frozen the snapshot yet.
 * (We assume that such statements cannot appear together with plannable
 * statements in the rewriter's output.)
 */
List *
pg_plan_queries(List *querytrees, ParamListInfo boundParams,
				bool needSnapshot)
{
	List	   *plan_list = NIL;
	ListCell   *query_list;

	foreach(query_list, querytrees)
	{
		Query	   *query = (Query *) lfirst(query_list);
		Plan	   *plan;

		if (query->commandType == CMD_UTILITY)
		{
			/* Utility commands have no plans. */
			plan = NULL;
		}
		else
		{
			if (needSnapshot)
			{
				ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());
				needSnapshot = false;
			}
			plan = pg_plan_query(query, boundParams);
		}

		plan_list = lappend(plan_list, plan);
	}

	return plan_list;
}


/*
 * exec_simple_query
 *
 * Execute a "simple Query" protocol message.
 */
static void
exec_simple_query(const char *query_string)
{
	CommandDest dest = whereToSendOutput;
	MemoryContext oldcontext;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	struct timeval start_t,
				stop_t;
	bool		save_log_duration = log_duration;
	int			save_log_min_duration_statement = log_min_duration_statement;
	bool		save_log_statement_stats = log_statement_stats;

	/*
	 * Report query to various monitoring facilities.
	 */
	debug_query_string = query_string;

	pgstat_report_activity(query_string);

	/*
	 * We use save_log_* so "SET log_duration = true"  and "SET
	 * log_min_duration_statement = true" don't report incorrect time
	 * because gettimeofday() wasn't called. Similarly,
	 * log_statement_stats has to be captured once.
	 */
	if (save_log_duration || save_log_min_duration_statement != -1)
		gettimeofday(&start_t, NULL);

	if (save_log_statement_stats)
		ResetUsage();

	/*
	 * Start up a transaction command.	All queries generated by the
	 * query_string will be in this same command block, *unless* we find a
	 * BEGIN/COMMIT/ABORT statement; we have to force a new xact command
	 * after one of those, else bad things will happen in xact.c. (Note
	 * that this will normally change current memory context.)
	 */
	start_xact_command();

	/*
	 * Zap any pre-existing unnamed statement.	(While not strictly
	 * necessary, it seems best to define simple-Query mode as if it used
	 * the unnamed statement and portal; this ensures we recover any
	 * storage used by prior unnamed operations.)
	 */
	unnamed_stmt_pstmt = NULL;
	if (unnamed_stmt_context)
	{
		DropDependentPortals(unnamed_stmt_context);
		MemoryContextDelete(unnamed_stmt_context);
	}
	unnamed_stmt_context = NULL;

	/*
	 * Switch to appropriate context for constructing parsetrees.
	 */
	oldcontext = MemoryContextSwitchTo(MessageContext);

	QueryContext = CurrentMemoryContext;

	/*
	 * Do basic parsing of the query or queries (this should be safe even
	 * if we are in aborted transaction state!)
	 */
	parsetree_list = pg_parse_query(query_string);

	/*
	 * Switch back to transaction context to enter the loop.
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Run through the raw parsetree(s) and process each one.
	 */
	foreach(parsetree_item, parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(parsetree_item);
		const char *commandTag;
		char		completionTag[COMPLETION_TAG_BUFSIZE];
		List	   *querytree_list,
				   *plantree_list;
		Portal		portal;
		DestReceiver *receiver;
		int16		format;

		/*
		 * Get the command name for use in status display (it also becomes
		 * the default completion tag, down inside PortalRun).	Set
		 * ps_status and do any special start-of-SQL-command processing
		 * needed by the destination.
		 */
		commandTag = CreateCommandTag(parsetree);

		set_ps_display(commandTag);

		BeginCommand(commandTag, dest);

		/*
		 * If we are in an aborted transaction, reject all commands except
		 * COMMIT/ABORT.  It is important that this test occur before we
		 * try to do parse analysis, rewrite, or planning, since all those
		 * phases try to do database accesses, which may fail in abort
		 * state. (It might be safe to allow some additional utility
		 * commands in this state, but not many...)
		 */
		if (IsAbortedTransactionBlockState())
		{
			bool		allowit = false;

			if (IsA(parsetree, TransactionStmt))
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				if (stmt->kind == TRANS_STMT_COMMIT ||
					stmt->kind == TRANS_STMT_ROLLBACK ||
					stmt->kind == TRANS_STMT_ROLLBACK_TO)
					allowit = true;
			}

			if (!allowit)
				ereport(ERROR,
						(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
						 errmsg("current transaction is aborted, "
					"commands ignored until end of transaction block")));
		}

		/* Make sure we are in a transaction command */
		start_xact_command();

		/* If we got a cancel signal in parsing or prior command, quit */
		CHECK_FOR_INTERRUPTS();

		/*
		 * OK to analyze, rewrite, and plan this query.
		 *
		 * Switch to appropriate context for constructing querytrees (again,
		 * these must outlive the execution context).
		 */
		oldcontext = MemoryContextSwitchTo(MessageContext);

		querytree_list = pg_analyze_and_rewrite(parsetree, NULL, 0);

		plantree_list = pg_plan_queries(querytree_list, NULL, true);

		/* If we got a cancel signal in analysis or planning, quit */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Create unnamed portal to run the query or queries in. If there
		 * already is one, silently drop it.
		 */
		portal = CreatePortal("", true, true);

		PortalDefineQuery(portal,
						  query_string,
						  commandTag,
						  querytree_list,
						  plantree_list,
						  MessageContext);

		/*
		 * Start the portal.  No parameters here.
		 */
		PortalStart(portal, NULL, InvalidSnapshot);

		/*
		 * Select the appropriate output format: text unless we are doing
		 * a FETCH from a binary cursor.  (Pretty grotty to have to do
		 * this here --- but it avoids grottiness in other places.	Ah,
		 * the joys of backward compatibility...)
		 */
		format = 0;				/* TEXT is default */
		if (IsA(parsetree, FetchStmt))
		{
			FetchStmt  *stmt = (FetchStmt *) parsetree;

			if (!stmt->ismove)
			{
				Portal		fportal = GetPortalByName(stmt->portalname);

				if (PortalIsValid(fportal) &&
					(fportal->cursorOptions & CURSOR_OPT_BINARY))
					format = 1; /* BINARY */
			}
		}
		PortalSetResultFormat(portal, 1, &format);

		/*
		 * Now we can create the destination receiver object.
		 */
		receiver = CreateDestReceiver(dest, portal);

		/*
		 * Switch back to transaction context for execution.
		 */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * Run the portal to completion, and then drop it (and the
		 * receiver).
		 */
		(void) PortalRun(portal,
						 FETCH_ALL,
						 receiver,
						 receiver,
						 completionTag);

		(*receiver->rDestroy) (receiver);

		PortalDrop(portal, false);

		if (IsA(parsetree, TransactionStmt))
		{
			/*
			 * If this was a transaction control statement, commit it. We
			 * will start a new xact command for the next command (if
			 * any).
			 */
			finish_xact_command();
		}
		else if (lnext(parsetree_item) == NULL)
		{
			/*
			 * If this is the last parsetree of the query string, close
			 * down transaction statement before reporting
			 * command-complete.  This is so that any end-of-transaction
			 * errors are reported before the command-complete message is
			 * issued, to avoid confusing clients who will expect either a
			 * command-complete message or an error, not one and then the
			 * other.  But for compatibility with historical Postgres
			 * behavior, we do not force a transaction boundary between
			 * queries appearing in a single query string.
			 */
			finish_xact_command();
		}
		else
		{
			/*
			 * We need a CommandCounterIncrement after every query, except
			 * those that start or end a transaction block.
			 */
			CommandCounterIncrement();
		}

		/*
		 * Tell client that we're done with this query.  Note we emit
		 * exactly one EndCommand report for each raw parsetree, thus one
		 * for each SQL command the client sent, regardless of rewriting.
		 * (But a command aborted by error will not send an EndCommand
		 * report at all.)
		 */
		EndCommand(completionTag, dest);
	}							/* end loop over parsetrees */

	/*
	 * Close down transaction statement, if one is open.
	 */
	finish_xact_command();

	/*
	 * If there were no parsetrees, return EmptyQueryResponse message.
	 */
	if (!parsetree_list)
		NullCommand(dest);

	QueryContext = NULL;

	/*
	 * Combine processing here as we need to calculate the query duration
	 * in both instances.
	 */
	if (save_log_duration || save_log_min_duration_statement != -1)
	{
		long		usecs;

		gettimeofday(&stop_t, NULL);
		if (stop_t.tv_usec < start_t.tv_usec)
		{
			stop_t.tv_sec--;
			stop_t.tv_usec += 1000000;
		}
		usecs = (long) (stop_t.tv_sec - start_t.tv_sec) * 1000000 + (long) (stop_t.tv_usec - start_t.tv_usec);

		if (save_log_duration)
			ereport(LOG,
					(errmsg("duration: %ld.%03ld ms",
						(long) ((stop_t.tv_sec - start_t.tv_sec) * 1000 +
							  (stop_t.tv_usec - start_t.tv_usec) / 1000),
					 (long) (stop_t.tv_usec - start_t.tv_usec) % 1000)));

		/*
		 * Output a duration_statement to the log if the query has
		 * exceeded the min duration, or if we are to print all durations.
		 */
		if (save_log_min_duration_statement == 0 ||
			(save_log_min_duration_statement > 0 &&
			 usecs >= save_log_min_duration_statement * 1000))
			ereport(LOG,
					(errmsg("duration: %ld.%03ld ms  statement: %s",
						(long) ((stop_t.tv_sec - start_t.tv_sec) * 1000 +
							  (stop_t.tv_usec - start_t.tv_usec) / 1000),
						(long) (stop_t.tv_usec - start_t.tv_usec) % 1000,
							query_string)));
	}

	if (save_log_statement_stats)
		ShowUsage("QUERY STATISTICS");

	debug_query_string = NULL;
}

/*
 * exec_parse_message
 *
 * Execute a "Parse" protocol message.
 */
static void
exec_parse_message(const char *query_string,	/* string to execute */
				   const char *stmt_name,		/* name for prepared stmt */
				   Oid *paramTypes,		/* parameter types */
				   int numParams)		/* number of parameters */
{
	MemoryContext oldcontext;
	List	   *parsetree_list;
	const char *commandTag;
	List	   *querytree_list,
			   *plantree_list,
			   *param_list;
	bool		is_named;
	bool		save_log_statement_stats = log_statement_stats;

	/*
	 * Report query to various monitoring facilities.
	 */
	debug_query_string = query_string;

	pgstat_report_activity(query_string);

	set_ps_display("PARSE");

	if (save_log_statement_stats)
		ResetUsage();

	/*
	 * Start up a transaction command so we can run parse analysis etc.
	 * (Note that this will normally change current memory context.)
	 * Nothing happens if we are already in one.
	 */
	start_xact_command();

	/*
	 * Switch to appropriate context for constructing parsetrees.
	 *
	 * We have two strategies depending on whether the prepared statement is
	 * named or not.  For a named prepared statement, we do parsing in
	 * MessageContext and copy the finished trees into the prepared
	 * statement's private context; then the reset of MessageContext
	 * releases temporary space used by parsing and planning.  For an
	 * unnamed prepared statement, we assume the statement isn't going to
	 * hang around long, so getting rid of temp space quickly is probably
	 * not worth the costs of copying parse/plan trees.  So in this case,
	 * we set up a special context for the unnamed statement, and do all
	 * the parsing/planning therein.
	 */
	is_named = (stmt_name[0] != '\0');
	if (is_named)
	{
		/* Named prepared statement --- parse in MessageContext */
		oldcontext = MemoryContextSwitchTo(MessageContext);
	}
	else
	{
		/* Unnamed prepared statement --- release any prior unnamed stmt */
		unnamed_stmt_pstmt = NULL;
		if (unnamed_stmt_context)
		{
			DropDependentPortals(unnamed_stmt_context);
			MemoryContextDelete(unnamed_stmt_context);
		}
		unnamed_stmt_context = NULL;
		/* create context for parsing/planning */
		unnamed_stmt_context =
			AllocSetContextCreate(TopMemoryContext,
								  "unnamed prepared statement",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
		oldcontext = MemoryContextSwitchTo(unnamed_stmt_context);
	}

	QueryContext = CurrentMemoryContext;

	/*
	 * Do basic parsing of the query or queries (this should be safe even
	 * if we are in aborted transaction state!)
	 */
	parsetree_list = pg_parse_query(query_string);

	/*
	 * We only allow a single user statement in a prepared statement. This
	 * is mainly to keep the protocol simple --- otherwise we'd need to
	 * worry about multiple result tupdescs and things like that.
	 */
	if (list_length(parsetree_list) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot insert multiple commands into a prepared statement")));

	if (parsetree_list != NIL)
	{
		Node	   *parsetree = (Node *) linitial(parsetree_list);
		int			i;

		/*
		 * Get the command name for possible use in status display.
		 */
		commandTag = CreateCommandTag(parsetree);

		/*
		 * If we are in an aborted transaction, reject all commands except
		 * COMMIT/ROLLBACK.  It is important that this test occur before
		 * we try to do parse analysis, rewrite, or planning, since all
		 * those phases try to do database accesses, which may fail in
		 * abort state. (It might be safe to allow some additional utility
		 * commands in this state, but not many...)
		 */
		if (IsAbortedTransactionBlockState())
		{
			bool		allowit = false;

			if (IsA(parsetree, TransactionStmt))
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				if (stmt->kind == TRANS_STMT_COMMIT ||
					stmt->kind == TRANS_STMT_ROLLBACK ||
					stmt->kind == TRANS_STMT_ROLLBACK_TO)
					allowit = true;
			}

			if (!allowit)
				ereport(ERROR,
						(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
						 errmsg("current transaction is aborted, "
					"commands ignored until end of transaction block")));
		}

		/*
		 * OK to analyze, rewrite, and plan this query.  Note that the
		 * originally specified parameter set is not required to be
		 * complete, so we have to use parse_analyze_varparams().
		 */
		if (log_parser_stats)
			ResetUsage();

		querytree_list = parse_analyze_varparams(parsetree,
												 &paramTypes,
												 &numParams);

		/*
		 * Check all parameter types got determined, and convert array
		 * representation to a list for storage.
		 */
		param_list = NIL;
		for (i = 0; i < numParams; i++)
		{
			Oid			ptype = paramTypes[i];

			if (ptype == InvalidOid || ptype == UNKNOWNOID)
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_DATATYPE),
				 errmsg("could not determine data type of parameter $%d",
						i + 1)));
			param_list = lappend_oid(param_list, ptype);
		}

		if (log_parser_stats)
			ShowUsage("PARSE ANALYSIS STATISTICS");

		querytree_list = pg_rewrite_queries(querytree_list);

		/*
		 * If this is the unnamed statement and it has parameters, defer
		 * query planning until Bind.  Otherwise do it now.
		 */
		if (!is_named && numParams > 0)
			plantree_list = NIL;
		else
			plantree_list = pg_plan_queries(querytree_list, NULL, true);
	}
	else
	{
		/* Empty input string.	This is legal. */
		commandTag = NULL;
		querytree_list = NIL;
		plantree_list = NIL;
		param_list = NIL;
	}

	/* If we got a cancel signal in analysis or planning, quit */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Store the query as a prepared statement.  See above comments.
	 */
	if (is_named)
	{
		StorePreparedStatement(stmt_name,
							   query_string,
							   commandTag,
							   querytree_list,
							   plantree_list,
							   param_list);
	}
	else
	{
		PreparedStatement *pstmt;

		pstmt = (PreparedStatement *) palloc0(sizeof(PreparedStatement));
		/* query_string needs to be copied into unnamed_stmt_context */
		pstmt->query_string = pstrdup(query_string);
		/* the rest is there already */
		pstmt->commandTag = commandTag;
		pstmt->query_list = querytree_list;
		pstmt->plan_list = plantree_list;
		pstmt->argtype_list = param_list;
		pstmt->context = unnamed_stmt_context;
		/* Now the unnamed statement is complete and valid */
		unnamed_stmt_pstmt = pstmt;
	}

	MemoryContextSwitchTo(oldcontext);

	QueryContext = NULL;

	/*
	 * We do NOT close the open transaction command here; that only
	 * happens when the client sends Sync.	Instead, do
	 * CommandCounterIncrement just in case something happened during
	 * parse/plan.
	 */
	CommandCounterIncrement();

	/*
	 * Send ParseComplete.
	 */
	if (whereToSendOutput == Remote)
		pq_putemptymessage('1');

	if (save_log_statement_stats)
		ShowUsage("PARSE MESSAGE STATISTICS");

	debug_query_string = NULL;
}

/*
 * exec_bind_message
 *
 * Process a "Bind" message to create a portal from a prepared statement
 */
static void
exec_bind_message(StringInfo input_message)
{
	const char *portal_name;
	const char *stmt_name;
	int			numPFormats;
	int16	   *pformats = NULL;
	int			numParams;
	int			numRFormats;
	int16	   *rformats = NULL;
	int			i;
	PreparedStatement *pstmt;
	Portal		portal;
	ParamListInfo params;
	bool		isaborted = IsAbortedTransactionBlockState();

	pgstat_report_activity("<BIND>");

	set_ps_display("BIND");

	/*
	 * Start up a transaction command so we can call functions etc. (Note
	 * that this will normally change current memory context.) Nothing
	 * happens if we are already in one.
	 */
	start_xact_command();

	/* Switch back to message context */
	MemoryContextSwitchTo(MessageContext);

	/* Get the fixed part of the message */
	portal_name = pq_getmsgstring(input_message);
	stmt_name = pq_getmsgstring(input_message);

	/* Get the parameter format codes */
	numPFormats = pq_getmsgint(input_message, 2);
	if (numPFormats > 0)
	{
		pformats = (int16 *) palloc(numPFormats * sizeof(int16));
		for (i = 0; i < numPFormats; i++)
			pformats[i] = pq_getmsgint(input_message, 2);
	}

	/* Get the parameter value count */
	numParams = pq_getmsgint(input_message, 2);

	if (numPFormats > 1 && numPFormats != numParams)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
		errmsg("bind message has %d parameter formats but %d parameters",
			   numPFormats, numParams)));

	/* Find prepared statement */
	if (stmt_name[0] != '\0')
		pstmt = FetchPreparedStatement(stmt_name, true);
	else
	{
		/* special-case the unnamed statement */
		pstmt = unnamed_stmt_pstmt;
		if (!pstmt)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
				   errmsg("unnamed prepared statement does not exist")));
	}

	if (numParams != list_length(pstmt->argtype_list))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("bind message supplies %d parameters, but prepared statement \"%s\" requires %d",
			   numParams, stmt_name, list_length(pstmt->argtype_list))));

	/*
	 * Create the portal.  Allow silent replacement of an existing portal
	 * only if the unnamed portal is specified.
	 */
	if (portal_name[0] == '\0')
		portal = CreatePortal(portal_name, true, true);
	else
		portal = CreatePortal(portal_name, false, false);

	/*
	 * Fetch parameters, if any, and store in the portal's memory context.
	 *
	 * In an aborted transaction, we can't risk calling user-defined
	 * functions, but we can't fail to Bind either, so bind all parameters
	 * to null values.
	 */
	if (numParams > 0)
	{
		ListCell   *l;
		MemoryContext oldContext;

		oldContext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

		params = (ParamListInfo)
			palloc0((numParams + 1) * sizeof(ParamListInfoData));

		i = 0;
		foreach(l, pstmt->argtype_list)
		{
			Oid			ptype = lfirst_oid(l);
			int32		plength;
			bool		isNull;

			plength = pq_getmsgint(input_message, 4);
			isNull = (plength == -1);

			if (!isNull)
			{
				const char *pvalue = pq_getmsgbytes(input_message, plength);

				if (isaborted)
				{
					/* We don't bother to check the format in this case */
					isNull = true;
				}
				else
				{
					int16		pformat;
					StringInfoData pbuf;
					char		csave;

					if (numPFormats > 1)
						pformat = pformats[i];
					else if (numPFormats > 0)
						pformat = pformats[0];
					else
						pformat = 0;	/* default = text */

					/*
					 * Rather than copying data around, we just set up a
					 * phony StringInfo pointing to the correct portion of
					 * the message buffer.	We assume we can scribble on
					 * the message buffer so as to maintain the convention
					 * that StringInfos have a trailing null.  This is
					 * grotty but is a big win when dealing with very
					 * large parameter strings.
					 */
					pbuf.data = (char *) pvalue;
					pbuf.maxlen = plength + 1;
					pbuf.len = plength;
					pbuf.cursor = 0;

					csave = pbuf.data[plength];
					pbuf.data[plength] = '\0';

					if (pformat == 0)
					{
						Oid			typinput;
						Oid			typioparam;
						char	   *pstring;

						getTypeInputInfo(ptype, &typinput, &typioparam);

						/*
						 * We have to do encoding conversion before
						 * calling the typinput routine.
						 */
						pstring = (char *)
							pg_client_to_server((unsigned char *) pbuf.data,
												plength);
						params[i].value =
							OidFunctionCall3(typinput,
											 CStringGetDatum(pstring),
											 ObjectIdGetDatum(typioparam),
											 Int32GetDatum(-1));
						/* Free result of encoding conversion, if any */
						if (pstring != pbuf.data)
							pfree(pstring);
					}
					else if (pformat == 1)
					{
						Oid			typreceive;
						Oid			typioparam;

						/*
						 * Call the parameter type's binary input
						 * converter
						 */
						getTypeBinaryInputInfo(ptype, &typreceive, &typioparam);

						params[i].value =
							OidFunctionCall2(typreceive,
											 PointerGetDatum(&pbuf),
										   ObjectIdGetDatum(typioparam));

						/* Trouble if it didn't eat the whole buffer */
						if (pbuf.cursor != pbuf.len)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
									 errmsg("incorrect binary data format in bind parameter %d",
											i + 1)));
					}
					else
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("unsupported format code: %d",
										pformat)));
					}

					/* Restore message buffer contents */
					pbuf.data[plength] = csave;
				}
			}

			params[i].kind = PARAM_NUM;
			params[i].id = i + 1;
			params[i].ptype = ptype;
			params[i].isnull = isNull;

			i++;
		}

		params[i].kind = PARAM_INVALID;

		MemoryContextSwitchTo(oldContext);
	}
	else
		params = NULL;

	/* Get the result format codes */
	numRFormats = pq_getmsgint(input_message, 2);
	if (numRFormats > 0)
	{
		rformats = (int16 *) palloc(numRFormats * sizeof(int16));
		for (i = 0; i < numRFormats; i++)
			rformats[i] = pq_getmsgint(input_message, 2);
	}

	pq_getmsgend(input_message);

	/*
	 * If we didn't plan the query before, do it now.  This allows the
	 * planner to make use of the concrete parameter values we now have.
	 *
	 * This happens only for unnamed statements, and so switching into the
	 * statement context for planning is correct (see notes in
	 * exec_parse_message).
	 */
	if (pstmt->plan_list == NIL && pstmt->query_list != NIL &&
		!isaborted)
	{
		MemoryContext oldContext = MemoryContextSwitchTo(pstmt->context);

		pstmt->plan_list = pg_plan_queries(pstmt->query_list, params, true);
		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * Define portal and start execution.
	 */
	PortalDefineQuery(portal,
					  pstmt->query_string,
					  pstmt->commandTag,
					  pstmt->query_list,
					  pstmt->plan_list,
					  pstmt->context);

	PortalStart(portal, params, InvalidSnapshot);

	/*
	 * Apply the result format requests to the portal.
	 */
	PortalSetResultFormat(portal, numRFormats, rformats);

	/*
	 * Send BindComplete.
	 */
	if (whereToSendOutput == Remote)
		pq_putemptymessage('2');
}

/*
 * exec_execute_message
 *
 * Process an "Execute" message for a portal
 */
static void
exec_execute_message(const char *portal_name, long max_rows)
{
	CommandDest dest;
	DestReceiver *receiver;
	Portal		portal;
	bool		is_trans_stmt = false;
	bool		is_trans_exit = false;
	bool		completed;
	char		completionTag[COMPLETION_TAG_BUFSIZE];

	/* Adjust destination to tell printtup.c what to do */
	dest = whereToSendOutput;
	if (dest == Remote)
		dest = RemoteExecute;

	portal = GetPortalByName(portal_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("portal \"%s\" does not exist", portal_name)));

	/*
	 * If the original query was a null string, just return
	 * EmptyQueryResponse.
	 */
	if (portal->commandTag == NULL)
	{
		Assert(portal->parseTrees == NIL);
		NullCommand(dest);
		return;
	}

	if (portal->sourceText)
	{
		debug_query_string = portal->sourceText;
		pgstat_report_activity(portal->sourceText);
	}
	else
	{
		debug_query_string = "execute message";
		pgstat_report_activity("<EXECUTE>");
	}

	set_ps_display(portal->commandTag);

	BeginCommand(portal->commandTag, dest);

	/* Check for transaction-control commands */
	if (list_length(portal->parseTrees) == 1)
	{
		Query	   *query = (Query *) linitial(portal->parseTrees);

		if (query->commandType == CMD_UTILITY &&
			query->utilityStmt != NULL &&
			IsA(query->utilityStmt, TransactionStmt))
		{
			TransactionStmt *stmt = (TransactionStmt *) query->utilityStmt;

			is_trans_stmt = true;
			if (stmt->kind == TRANS_STMT_COMMIT ||
				stmt->kind == TRANS_STMT_ROLLBACK ||
				stmt->kind == TRANS_STMT_ROLLBACK_TO)
				is_trans_exit = true;
		}
	}

	/*
	 * Create dest receiver in MessageContext (we don't want it in
	 * transaction context, because that may get deleted if portal
	 * contains VACUUM).
	 */
	receiver = CreateDestReceiver(dest, portal);

	/*
	 * Ensure we are in a transaction command (this should normally be the
	 * case already due to prior BIND).
	 */
	start_xact_command();

	/*
	 * If we are in aborted transaction state, the only portals we can
	 * actually run are those containing COMMIT or ROLLBACK commands.
	 */
	if (IsAbortedTransactionBlockState())
	{
		if (!is_trans_exit)
			ereport(ERROR,
					(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
					 errmsg("current transaction is aborted, "
					"commands ignored until end of transaction block")));
	}

	/* Check for cancel signal before we start execution */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Okay to run the portal.
	 */
	if (max_rows <= 0)
		max_rows = FETCH_ALL;

	completed = PortalRun(portal,
						  max_rows,
						  receiver,
						  receiver,
						  completionTag);

	(*receiver->rDestroy) (receiver);

	if (completed)
	{
		if (is_trans_stmt)
		{
			/*
			 * If this was a transaction control statement, commit it.	We
			 * will start a new xact command for the next command (if
			 * any).
			 */
			finish_xact_command();
		}
		else
		{
			/*
			 * We need a CommandCounterIncrement after every query, except
			 * those that start or end a transaction block.
			 */
			CommandCounterIncrement();
		}

		/* Send appropriate CommandComplete to client */
		EndCommand(completionTag, dest);
	}
	else
	{
		/* Portal run not complete, so send PortalSuspended */
		if (whereToSendOutput == Remote)
			pq_putemptymessage('s');
	}

	debug_query_string = NULL;
}

/*
 * exec_describe_statement_message
 *
 * Process a "Describe" message for a prepared statement
 */
static void
exec_describe_statement_message(const char *stmt_name)
{
	PreparedStatement *pstmt;
	TupleDesc	tupdesc;
	ListCell   *l;
	StringInfoData buf;

	/* Find prepared statement */
	if (stmt_name[0] != '\0')
		pstmt = FetchPreparedStatement(stmt_name, true);
	else
	{
		/* special-case the unnamed statement */
		pstmt = unnamed_stmt_pstmt;
		if (!pstmt)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
				   errmsg("unnamed prepared statement does not exist")));
	}

	if (whereToSendOutput != Remote)
		return;					/* can't actually do anything... */

	/*
	 * First describe the parameters...
	 */
	pq_beginmessage(&buf, 't'); /* parameter description message type */
	pq_sendint(&buf, list_length(pstmt->argtype_list), 2);

	foreach(l, pstmt->argtype_list)
	{
		Oid			ptype = lfirst_oid(l);

		pq_sendint(&buf, (int) ptype, 4);
	}
	pq_endmessage(&buf);

	/*
	 * Next send RowDescription or NoData to describe the result...
	 */
	tupdesc = FetchPreparedStatementResultDesc(pstmt);
	if (tupdesc)
	{
		List	   *targetlist;

		if (ChoosePortalStrategy(pstmt->query_list) == PORTAL_ONE_SELECT)
			targetlist = ((Query *) linitial(pstmt->query_list))->targetList;
		else
			targetlist = NIL;
		SendRowDescriptionMessage(tupdesc, targetlist, NULL);
	}
	else
		pq_putemptymessage('n');	/* NoData */

}

/*
 * exec_describe_portal_message
 *
 * Process a "Describe" message for a portal
 */
static void
exec_describe_portal_message(const char *portal_name)
{
	Portal		portal;

	portal = GetPortalByName(portal_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("portal \"%s\" does not exist", portal_name)));

	if (whereToSendOutput != Remote)
		return;					/* can't actually do anything... */

	if (portal->tupDesc)
	{
		List	   *targetlist;

		if (portal->strategy == PORTAL_ONE_SELECT)
			targetlist = ((Query *) linitial(portal->parseTrees))->targetList;
		else
			targetlist = NIL;
		SendRowDescriptionMessage(portal->tupDesc, targetlist,
								  portal->formats);
	}
	else
		pq_putemptymessage('n');	/* NoData */
}


/*
 * Convenience routines for starting/committing a single command.
 */
static void
start_xact_command(void)
{
	if (!xact_started)
	{
		ereport(DEBUG3,
				(errmsg_internal("StartTransactionCommand")));
		StartTransactionCommand();

		/* Set statement timeout running, if any */
		if (StatementTimeout > 0)
			enable_sig_alarm(StatementTimeout, true);

		xact_started = true;
	}
}

static void
finish_xact_command(void)
{
	if (xact_started)
	{
		/* Cancel any active statement timeout before committing */
		disable_sig_alarm(true);

		/* Now commit the command */
		ereport(DEBUG3,
				(errmsg_internal("CommitTransactionCommand")));

		CommitTransactionCommand();

#ifdef MEMORY_CONTEXT_CHECKING
		/* Check all memory contexts that weren't freed during commit */
		/* (those that were, were checked before being deleted) */
		MemoryContextCheck(TopMemoryContext);
#endif

#ifdef SHOW_MEMORY_STATS
		/* Print mem stats after each commit for leak tracking */
		if (ShowStats)
			MemoryContextStats(TopMemoryContext);
#endif

		xact_started = false;
	}
}


/* --------------------------------
 *		signal handler routines used in PostgresMain()
 * --------------------------------
 */

/*
 * quickdie() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm,
 * so we need to stop what we're doing and exit.
 */
void
quickdie(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);

	/*
	 * Ideally this should be ereport(FATAL), but then we'd not get
	 * control back...
	 */
	ereport(WARNING,
			(errcode(ERRCODE_CRASH_SHUTDOWN),
			 errmsg("terminating connection because of crash of another server process"),
			 errdetail("The postmaster has commanded this server process to roll back"
					 " the current transaction and exit, because another"
			   " server process exited abnormally and possibly corrupted"
					   " shared memory."),
			 errhint("In a moment you should be able to reconnect to the"
					 " database and repeat your command.")));

	/*
	 * DO NOT proc_exit() -- we're here because shared memory may be
	 * corrupted, so we don't want to try to clean up our transaction.
	 * Just nail the windows shut and get out of town.
	 *
	 * Note we do exit(1) not exit(0).	This is to force the postmaster into
	 * a system reset cycle if some idiot DBA sends a manual SIGQUIT to a
	 * random backend.	This is necessary precisely because we don't clean
	 * up our shared memory state.
	 */
	exit(1);
}

/*
 * Shutdown signal from postmaster: abort transaction and exit
 * at soonest convenient time
 */
void
die(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/* Don't joggle the elbow of proc_exit */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		ProcDiePending = true;

		/*
		 * If it's safe to interrupt, and we're waiting for input or a
		 * lock, service the interrupt immediately
		 */
		if (ImmediateInterruptOK && InterruptHoldoffCount == 0 &&
			CritSectionCount == 0)
		{
			/* bump holdoff count to make ProcessInterrupts() a no-op */
			/* until we are done getting ready for it */
			InterruptHoldoffCount++;
			DisableNotifyInterrupt();
			DisableCatchupInterrupt();
			/* Make sure CheckDeadLock won't run while shutting down... */
			LockWaitCancel();
			InterruptHoldoffCount--;
			ProcessInterrupts();
		}
	}

	errno = save_errno;
}

/*
 * Timeout or shutdown signal from postmaster during client authentication.
 * Simply exit(0).
 *
 * XXX: possible future improvement: try to send a message indicating
 * why we are disconnecting.  Problem is to be sure we don't block while
 * doing so, nor mess up the authentication message exchange.
 */
void
authdie(SIGNAL_ARGS)
{
	exit(0);
}

/*
 * Query-cancel signal from postmaster: abort current transaction
 * at soonest convenient time
 */
static void
StatementCancelHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * Don't joggle the elbow of proc_exit
	 */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		QueryCancelPending = true;

		/*
		 * If it's safe to interrupt, and we're waiting for a lock,
		 * service the interrupt immediately.  No point in interrupting if
		 * we're waiting for input, however.
		 */
		if (ImmediateInterruptOK && InterruptHoldoffCount == 0 &&
			CritSectionCount == 0)
		{
			/* bump holdoff count to make ProcessInterrupts() a no-op */
			/* until we are done getting ready for it */
			InterruptHoldoffCount++;
			if (LockWaitCancel())
			{
				DisableNotifyInterrupt();
				DisableCatchupInterrupt();
				InterruptHoldoffCount--;
				ProcessInterrupts();
			}
			else
				InterruptHoldoffCount--;
		}
	}

	errno = save_errno;
}

/* signal handler for floating point exception */
static void
FloatExceptionHandler(SIGNAL_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FLOATING_POINT_EXCEPTION),
			 errmsg("floating-point exception"),
		   errdetail("An invalid floating-point operation was signaled. "
					 "This probably means an out-of-range result or an "
					 "invalid operation, such as division by zero.")));
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
SigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
}


/*
 * ProcessInterrupts: out-of-line portion of CHECK_FOR_INTERRUPTS() macro
 *
 * If an interrupt condition is pending, and it's safe to service it,
 * then clear the flag and accept the interrupt.  Called only when
 * InterruptPending is true.
 */
void
ProcessInterrupts(void)
{
	/* OK to accept interrupt now? */
	if (InterruptHoldoffCount != 0 || CritSectionCount != 0)
		return;
	InterruptPending = false;
	if (ProcDiePending)
	{
		ProcDiePending = false;
		QueryCancelPending = false;		/* ProcDie trumps QueryCancel */
		ImmediateInterruptOK = false;	/* not idle anymore */
		DisableNotifyInterrupt();
		DisableCatchupInterrupt();
		ereport(FATAL,
				(errcode(ERRCODE_ADMIN_SHUTDOWN),
		 errmsg("terminating connection due to administrator command")));
	}
	if (QueryCancelPending)
	{
		QueryCancelPending = false;
		ImmediateInterruptOK = false;	/* not idle anymore */
		DisableNotifyInterrupt();
		DisableCatchupInterrupt();
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("canceling query due to user request")));
	}
	/* If we get here, do nothing (probably, QueryCancelPending was reset) */
}


/*
 * check_stack_depth: check for excessively deep recursion
 *
 * This should be called someplace in any recursive routine that might possibly
 * recurse deep enough to overflow the stack.  Most Unixen treat stack
 * overflow as an unrecoverable SIGSEGV, so we want to error out ourselves
 * before hitting the hardware limit.  Unfortunately we have no direct way
 * to detect the hardware limit, so we have to rely on the admin to set a
 * GUC variable for it ...
 */
void
check_stack_depth(void)
{
	char		stack_top_loc;
	int			stack_depth;

	/*
	 * Compute distance from PostgresMain's local variables to my own
	 *
	 * Note: in theory stack_depth should be ptrdiff_t or some such, but
	 * since the whole point of this code is to bound the value to
	 * something much less than integer-sized, int should work fine.
	 */
	stack_depth = (int) (stack_base_ptr - &stack_top_loc);

	/*
	 * Take abs value, since stacks grow up on some machines, down on
	 * others
	 */
	if (stack_depth < 0)
		stack_depth = -stack_depth;

	/*
	 * Trouble?
	 *
	 * The test on stack_base_ptr prevents us from erroring out if called
	 * during process setup or in a non-backend process.  Logically it
	 * should be done first, but putting it here avoids wasting cycles
	 * during normal cases.
	 */
	if (stack_depth > max_stack_depth_bytes &&
		stack_base_ptr != NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
				 errmsg("stack depth limit exceeded"),
				 errhint("Increase the configuration parameter \"max_stack_depth\".")));
	}
}

/* GUC assign hook to update max_stack_depth_bytes from max_stack_depth */
bool
assign_max_stack_depth(int newval, bool doit, GucSource source)
{
	/* Range check was already handled by guc.c */
	if (doit)
		max_stack_depth_bytes = newval * 1024;
	return true;
}


static void
usage(const char *progname)
{
	printf(gettext("%s is the PostgreSQL stand-alone backend.  It is not\nintended to be used by normal users.\n\n"), progname);

	printf(gettext("Usage:\n  %s [OPTION]... [DBNAME]\n\n"), progname);
	printf(gettext("Options:\n"));
#ifdef USE_ASSERT_CHECKING
	printf(gettext("  -A 1|0          enable/disable run-time assert checking\n"));
#endif
	printf(gettext("  -B NBUFFERS     number of shared buffers\n"));
	printf(gettext("  -c NAME=VALUE   set run-time parameter\n"));
	printf(gettext("  -d 0-5          debugging level (0 is off)\n"));
	printf(gettext("  -D DATADIR      database directory\n"));
	printf(gettext("  -e              use European date input format (DMY)\n"));
	printf(gettext("  -E              echo query before execution\n"));
	printf(gettext("  -F              turn fsync off\n"));
	printf(gettext("  -N              do not use newline as interactive query delimiter\n"));
	printf(gettext("  -o FILENAME     send stdout and stderr to given file\n"));
	printf(gettext("  -P              disable system indexes\n"));
	printf(gettext("  -s              show statistics after each query\n"));
	printf(gettext("  -S WORK-MEM     set amount of memory for sorts (in kbytes)\n"));
	printf(gettext("  --describe-config  describe configuration parameters, then exit\n"));
	printf(gettext("  --help          show this help, then exit\n"));
	printf(gettext("  --version       output version information, then exit\n"));
	printf(gettext("\nDeveloper options:\n"));
	printf(gettext("  -f s|i|n|m|h    forbid use of some plan types\n"));
	printf(gettext("  -i              do not execute queries\n"));
	printf(gettext("  -O              allow system table structure changes\n"));
	printf(gettext("  -t pa|pl|ex     show timings after each query\n"));
	printf(gettext("  -W NUM          wait NUM seconds to allow attach from a debugger\n"));
	printf(gettext("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}


/* ----------------------------------------------------------------
 * PostgresMain
 *	   postgres main loop -- all backends, interactive or otherwise start here
 *
 * argc/argv are the command line arguments to be used.  (When being forked
 * by the postmaster, these are not the original argv array of the process.)
 * username is the (possibly authenticated) PostgreSQL user name to be used
 * for the session.
 * ----------------------------------------------------------------
 */
int
PostgresMain(int argc, char *argv[], const char *username)
{
	int			flag;
	const char *dbname = NULL;
	char	   *userPGDATA = NULL;
	bool		secure;
	int			errs = 0;
	int			debug_flag = 0;
	GucContext	ctx,
				debug_context;
	GucSource	gucsource;
	char	   *tmp;
	int			firstchar;
	char		stack_base;
	StringInfoData input_message;
	sigjmp_buf	local_sigjmp_buf;
	volatile bool send_rfq = true;

	/*
	 * Catch standard options before doing much else.  This even works on
	 * systems without getopt_long.
	 */
	if (!IsUnderPostmaster && argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(argv[0]);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts(PG_VERSIONSTR);
			exit(0);
		}
	}

	/*
	 * initialize globals (already done if under postmaster, but not if
	 * standalone; cheap enough to do over)
	 */
	MyProcPid = getpid();

	/*
	 * Fire up essential subsystems: error and memory management
	 *
	 * If we are running under the postmaster, this is done already.
	 */
	if (!IsUnderPostmaster)
		MemoryContextInit();

	set_ps_display("startup");

	SetProcessingMode(InitProcessing);

	/* Set up reference point for stack depth checking */
	stack_base_ptr = &stack_base;

	/* Compute paths, if we didn't inherit them from postmaster */
	if (my_exec_path[0] == '\0')
	{
		if (find_my_exec(argv[0], my_exec_path) < 0)
			elog(FATAL, "%s: could not locate my own executable path",
				 argv[0]);
	}

	if (pkglib_path[0] == '\0')
		get_pkglib_path(my_exec_path, pkglib_path);

	/*
	 * Set default values for command-line options.
	 */
	EchoQuery = false;

	if (!IsUnderPostmaster)
	{
		InitializeGUCOptions();
		userPGDATA = getenv("PGDATA");
	}

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

	/* all options are allowed until '-p' */
	secure = true;
	ctx = debug_context = PGC_POSTMASTER;
	gucsource = PGC_S_ARGV;		/* initial switches came from command line */

	while ((flag = getopt(argc, argv, "A:B:c:D:d:Eef:FiNOPo:p:S:st:v:W:-:")) != -1)
		switch (flag)
		{
			case 'A':
#ifdef USE_ASSERT_CHECKING
				SetConfigOption("debug_assertions", optarg, ctx, gucsource);
#else
				ereport(WARNING,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("assert checking is not compiled in")));
#endif
				break;

			case 'B':

				/*
				 * specify the size of buffer pool
				 */
				SetConfigOption("shared_buffers", optarg, ctx, gucsource);
				break;

			case 'D':			/* PGDATA directory */
				if (secure)
					userPGDATA = optarg;
				break;

			case 'd':			/* debug level */
				{
					/*
					 * Client option can't decrease debug level. We have
					 * to do the test here because we group priv and
					 * client set GUC calls below, after we know the final
					 * debug value.
					 */
					if (ctx != PGC_BACKEND || atoi(optarg) > debug_flag)
					{
						debug_flag = atoi(optarg);
						debug_context = ctx;	/* save context for use
												 * below */
						/* Set server debugging level. */
						if (debug_flag != 0)
						{
							char	   *debugstr = palloc(strlen("debug") + strlen(optarg) + 1);

							sprintf(debugstr, "debug%s", optarg);
							SetConfigOption("log_min_messages", debugstr, ctx, gucsource);
							pfree(debugstr);

						}
						else

							/*
							 * -d0 allows user to prevent postmaster debug
							 * from propagating to backend.  It would be
							 * nice to set it to the postgresql.conf value
							 * here.
							 */
							SetConfigOption("log_min_messages", "notice",
											ctx, gucsource);
					}
				}
				break;

			case 'E':

				/*
				 * E - echo the query the user entered
				 */
				EchoQuery = true;
				break;

			case 'e':

				/*
				 * Use European date input format (DMY)
				 */
				SetConfigOption("datestyle", "euro", ctx, gucsource);
				break;

			case 'F':

				/*
				 * turn off fsync
				 */
				SetConfigOption("fsync", "false", ctx, gucsource);
				break;

			case 'f':

				/*
				 * f - forbid generation of certain plans
				 */
				tmp = NULL;
				switch (optarg[0])
				{
					case 's':	/* seqscan */
						tmp = "enable_seqscan";
						break;
					case 'i':	/* indexscan */
						tmp = "enable_indexscan";
						break;
					case 't':	/* tidscan */
						tmp = "enable_tidscan";
						break;
					case 'n':	/* nestloop */
						tmp = "enable_nestloop";
						break;
					case 'm':	/* mergejoin */
						tmp = "enable_mergejoin";
						break;
					case 'h':	/* hashjoin */
						tmp = "enable_hashjoin";
						break;
					default:
						errs++;
				}
				if (tmp)
					SetConfigOption(tmp, "false", ctx, gucsource);
				break;

			case 'N':

				/*
				 * N - Don't use newline as a query delimiter
				 */
				UseNewLine = 0;
				break;

			case 'O':

				/*
				 * allow system table structure modifications
				 */
				if (secure)		/* XXX safe to allow from client??? */
					allowSystemTableMods = true;
				break;

			case 'P':

				/*
				 * ignore system indexes
				 *
				 * As of PG 7.4 this is safe to allow from the client, since
				 * it only disables reading the system indexes, not
				 * writing them.  Worst case consequence is slowness.
				 */
				IgnoreSystemIndexes(true);
				break;

			case 'o':

				/*
				 * o - send output (stdout and stderr) to the given file
				 */
				if (secure)
					StrNCpy(OutputFileName, optarg, MAXPGPATH);
				break;

			case 'p':

				/*
				 * p - special flag passed if backend was forked by a
				 * postmaster.
				 */
				if (secure)
				{
					dbname = strdup(optarg);

					secure = false;		/* subsequent switches are NOT
										 * secure */
					ctx = PGC_BACKEND;
					gucsource = PGC_S_CLIENT;
				}
				break;

			case 'S':

				/*
				 * S - amount of sort memory to use in 1k bytes
				 */
				SetConfigOption("work_mem", optarg, ctx, gucsource);
				break;

			case 's':

				/*
				 * s - report usage statistics (timings) after each query
				 */
				SetConfigOption("log_statement_stats", "true", ctx, gucsource);
				break;

			case 't':
				/* ---------------
				 *	tell postgres to report usage statistics (timings) for
				 *	each query
				 *
				 *	-tpa[rser] = print stats for parser time of each query
				 *	-tpl[anner] = print stats for planner time of each query
				 *	-te[xecutor] = print stats for executor time of each query
				 *	caution: -s can not be used together with -t.
				 * ----------------
				 */
				tmp = NULL;
				switch (optarg[0])
				{
					case 'p':
						if (optarg[1] == 'a')
							tmp = "log_parser_stats";
						else if (optarg[1] == 'l')
							tmp = "log_planner_stats";
						else
							errs++;
						break;
					case 'e':
						tmp = "log_executor_stats";
						break;
					default:
						errs++;
						break;
				}
				if (tmp)
					SetConfigOption(tmp, "true", ctx, gucsource);
				break;

			case 'v':
				if (secure)
					FrontendProtocol = (ProtocolVersion) atoi(optarg);
				break;

			case 'W':

				/*
				 * wait N seconds to allow attach from a debugger
				 */
				pg_usleep(atoi(optarg) * 1000000L);
				break;

			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (flag == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}

					SetConfigOption(name, value, ctx, gucsource);
					free(name);
					if (value)
						free(value);
					break;
				}

			default:
				errs++;
				break;
		}


	/*
	 * -d is not the same as setting log_min_messages because it enables
	 * other output options.
	 */
	if (debug_flag >= 1)
	{
		SetConfigOption("log_connections", "true", debug_context, gucsource);
		SetConfigOption("log_disconnections", "true", debug_context, gucsource);
	}
	if (debug_flag >= 2)
		SetConfigOption("log_statement", "all", debug_context, gucsource);
	if (debug_flag >= 3)
		SetConfigOption("debug_print_parse", "true", debug_context, gucsource);
	if (debug_flag >= 4)
		SetConfigOption("debug_print_plan", "true", debug_context, gucsource);
	if (debug_flag >= 5)
		SetConfigOption("debug_print_rewritten", "true", debug_context, gucsource);

	/*
	 * Process any additional GUC variable settings passed in startup
	 * packet.
	 */
	if (MyProcPort != NULL)
	{
		ListCell   *gucopts = list_head(MyProcPort->guc_options);

		while (gucopts)
		{
			char	   *name;
			char	   *value;

			name = lfirst(gucopts);
			gucopts = lnext(gucopts);

			value = lfirst(gucopts);
			gucopts = lnext(gucopts);

			SetConfigOption(name, value, PGC_BACKEND, PGC_S_CLIENT);
		}

		/*
		 * set up handler to log session end.
		 */
		if (IsUnderPostmaster && Log_disconnections)
			on_proc_exit(log_disconnections, 0);
	}

	if (!IsUnderPostmaster)
	{
		if (!userPGDATA)
		{
			write_stderr("%s does not know where to find the database system data.\n"
						 "You must specify the directory that contains the database system\n"
						 "either by specifying the -D invocation option or by setting the\n"
						 "PGDATA environment variable.\n",
						 argv[0]);
			proc_exit(1);
		}
		SetDataDir(userPGDATA);
	}
	Assert(DataDir);

	/* Acquire configuration parameters, unless inherited from postmaster */
	if (!IsUnderPostmaster)
	{
		ProcessConfigFile(PGC_POSTMASTER);

		/* If timezone is not set, determine what the OS uses */
		pg_timezone_initialize();
	}

	/*
	 * Set up signal handlers and masks.
	 *
	 * Note that postmaster blocked all signals before forking child process,
	 * so there is no race condition whereby we might receive a signal
	 * before we have set up the handler.
	 *
	 * Also note: it's best not to use any signals that are SIG_IGNored in
	 * the postmaster.	If such a signal arrives before we are able to
	 * change the handler to non-SIG_IGN, it'll get dropped.  Instead,
	 * make a dummy handler in the postmaster to reserve the signal. (Of
	 * course, this isn't an issue for signals that are locally generated,
	 * such as SIGALRM and SIGPIPE.)
	 */
	pqsignal(SIGHUP, SigHupHandler);	/* set flag to read config file */
	pqsignal(SIGINT, StatementCancelHandler);	/* cancel current query */
	pqsignal(SIGTERM, die);		/* cancel current query and exit */
	pqsignal(SIGQUIT, quickdie);	/* hard crash time */
	pqsignal(SIGALRM, handle_sig_alarm);		/* timeout conditions */

	/*
	 * Ignore failure to write to frontend. Note: if frontend closes
	 * connection, we will notice it and exit cleanly when control next
	 * returns to outer loop.  This seems safer than forcing exit in the
	 * midst of output during who-knows-what operation...
	 */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, CatchupInterruptHandler);
	pqsignal(SIGUSR2, NotifyInterruptHandler);
	pqsignal(SIGFPE, FloatExceptionHandler);

	/*
	 * Reset some signals that are accepted by postmaster but not by
	 * backend
	 */
	pqsignal(SIGCHLD, SIG_DFL); /* system() requires this on some
								 * platforms */

	pqinitmask();

	/* We allow SIGQUIT (quickdie) at all times */
#ifdef HAVE_SIGPROCMASK
	sigdelset(&BlockSig, SIGQUIT);
#else
	BlockSig &= ~(sigmask(SIGQUIT));
#endif

	PG_SETMASK(&BlockSig);		/* block everything except SIGQUIT */


	if (IsUnderPostmaster)
	{
		/* noninteractive case: nothing should be left after switches */
		if (errs || argc != optind || dbname == NULL)
		{
			ereport(FATAL,
					(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid command-line arguments for server process"),
			errhint("Try \"%s --help\" for more information.", argv[0])));
		}

		XLOGPathInit();

		BaseInit();
	}
	else
	{
		/* interactive case: database name can be last arg on command line */
		if (errs || argc - optind > 1)
		{
			ereport(FATAL,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s: invalid command-line arguments",
							argv[0]),
			errhint("Try \"%s --help\" for more information.", argv[0])));
		}
		else if (argc - optind == 1)
			dbname = argv[optind];
		else if ((dbname = username) == NULL)
		{
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s: no database nor user name specified",
							argv[0])));
		}

		/*
		 * Validate we have been given a reasonable-looking DataDir (if
		 * under postmaster, assume postmaster did this already).
		 */
		ValidatePgVersion(DataDir);

		/*
		 * Create lockfile for data directory.
		 */
		CreateDataDirLockFile(DataDir, false);

		XLOGPathInit();
		BaseInit();

		/*
		 * Start up xlog for standalone backend, and register to have it
		 * closed down at exit.
		 */
		StartupXLOG();
		on_shmem_exit(ShutdownXLOG, 0);

		/*
		 * Read any existing FSM cache file, and register to write one out
		 * at exit.
		 */
		LoadFreeSpaceMap();
		on_shmem_exit(DumpFreeSpaceMap, 0);
	}

	/*
	 * General initialization.
	 *
	 * NOTE: if you are tempted to add code in this vicinity, consider
	 * putting it inside InitPostgres() instead.  In particular, anything
	 * that involves database access should be there, not here.
	 */
	ereport(DEBUG3,
			(errmsg_internal("InitPostgres")));
	InitPostgres(dbname, username);

	SetProcessingMode(NormalProcessing);

	/*
	 * Send this backend's cancellation info to the frontend.
	 */
	if (whereToSendOutput == Remote &&
		PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, 'K');
		pq_sendint(&buf, (int32) MyProcPid, sizeof(int32));
		pq_sendint(&buf, (int32) MyCancelKey, sizeof(int32));
		pq_endmessage(&buf);
		/* Need not flush since ReadyForQuery will do it. */
	}

	/* Welcome banner for standalone case */
	if (whereToSendOutput == Debug)
		printf("\nPostgreSQL stand-alone backend %s\n", PG_VERSION);

	/*
	 * Create the memory context we will use in the main loop.
	 *
	 * MessageContext is reset once per iteration of the main loop, ie, upon
	 * completion of processing of each command message from the client.
	 */
	MessageContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);

	/* ----------
	 * Tell the statistics collector that we're alive and
	 * to which database we belong.
	 * ----------
	 */
	pgstat_bestart();

	/*
	 * POSTGRES main processing loop begins here
	 *
	 * If an exception is encountered, processing resumes here so we abort
	 * the current transaction and start a new one.
	 *
	 * You might wonder why this isn't coded as an infinite loop around a
	 * PG_TRY construct.  The reason is that this is the bottom of the
	 * exception stack, and so with PG_TRY there would be no exception
	 * handler in force at all during the CATCH part.  By leaving the
	 * outermost setjmp always active, we have at least some chance of
	 * recovering from an error during error recovery.	(If we get into an
	 * infinite loop thereby, it will soon be stopped by overflow of
	 * elog.c's internal state stack.)
	 */

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/*
		 * NOTE: if you are tempted to add more code in this if-block,
		 * consider the high probability that it should be in
		 * AbortTransaction() instead.	The only stuff done directly here
		 * should be stuff that is guaranteed to apply *only* for
		 * outer-level error recovery, such as adjusting the FE/BE
		 * protocol status.
		 */

		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/*
		 * Forget any pending QueryCancel request, since we're returning
		 * to the idle loop anyway, and cancel the statement timer if
		 * running.
		 */
		QueryCancelPending = false;
		disable_sig_alarm(true);
		QueryCancelPending = false;		/* again in case timeout occurred */

		/*
		 * Turn off these interrupts too.  This is only needed here and
		 * not in other exception-catching places since these interrupts
		 * are only enabled while we wait for client input.
		 */
		DisableNotifyInterrupt();
		DisableCatchupInterrupt();

		/* Make sure libpq is in a good state */
		pq_comm_reset();

		/* Report the error to the client and/or server log */
		EmitErrorReport();

		/*
		 * Make sure debug_query_string gets reset before we possibly
		 * clobber the storage it points at.
		 */
		debug_query_string = NULL;

		/*
		 * Abort the current transaction in order to recover.
		 */
		AbortCurrentTransaction();

		/*
		 * Now return to normal top-level context and clear ErrorContext
		 * for next time.
		 */
		MemoryContextSwitchTo(TopMemoryContext);
		FlushErrorState();
		QueryContext = NULL;

		/*
		 * If we were handling an extended-query-protocol message,
		 * initiate skip till next Sync.  This also causes us not to issue
		 * ReadyForQuery (until we get Sync).
		 */
		if (doing_extended_query_message)
			ignore_till_sync = true;

		/* We don't have a transaction command open anymore */
		xact_started = false;

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	PG_SETMASK(&UnBlockSig);

	if (!ignore_till_sync)
		send_rfq = true;		/* initially, or after error */

	/*
	 * Non-error queries loop here.
	 */

	for (;;)
	{
		/*
		 * At top of loop, reset extended-query-message flag, so that any
		 * errors encountered in "idle" state don't provoke skip.
		 */
		doing_extended_query_message = false;

		/*
		 * Release storage left over from prior query cycle, and create a
		 * new query input buffer in the cleared MessageContext.
		 */
		MemoryContextSwitchTo(MessageContext);
		MemoryContextResetAndDeleteChildren(MessageContext);

		initStringInfo(&input_message);

		/*
		 * (1) If we've reached idle state, tell the frontend we're ready
		 * for a new query.
		 *
		 * Note: this includes fflush()'ing the last of the prior output.
		 *
		 * This is also a good time to send collected statistics to the
		 * collector, and to update the PS stats display.  We avoid doing
		 * those every time through the message loop because it'd slow
		 * down processing of batched messages.
		 */
		if (send_rfq)
		{
			pgstat_report_tabstat();

			if (IsTransactionOrTransactionBlock())
			{
				set_ps_display("idle in transaction");
				pgstat_report_activity("<IDLE> in transaction");
			}
			else
			{
				set_ps_display("idle");
				pgstat_report_activity("<IDLE>");
			}

			ReadyForQuery(whereToSendOutput);
			send_rfq = false;
		}

		/*
		 * (2) deal with pending asynchronous NOTIFY from other backends,
		 * and enable async.c's signal handler to execute NOTIFY directly.
		 * Then set up other stuff needed before blocking for input.
		 */
		QueryCancelPending = false;		/* forget any earlier CANCEL
										 * signal */

		EnableNotifyInterrupt();
		EnableCatchupInterrupt();

		/* Allow "die" interrupt to be processed while waiting */
		ImmediateInterruptOK = true;
		/* and don't forget to detect one that already arrived */
		QueryCancelPending = false;
		CHECK_FOR_INTERRUPTS();

		/*
		 * (3) read a command (loop blocks here)
		 */
		firstchar = ReadCommand(&input_message);

		/*
		 * (4) disable async signal conditions again.
		 */
		ImmediateInterruptOK = false;
		QueryCancelPending = false;		/* forget any CANCEL signal */

		DisableNotifyInterrupt();
		DisableCatchupInterrupt();

		/*
		 * (5) check for any other interesting events that happened while
		 * we slept.
		 */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * (6) process the command.  But ignore it if we're skipping till
		 * Sync.
		 */
		if (ignore_till_sync && firstchar != EOF)
			continue;

		switch (firstchar)
		{
			case 'Q':			/* simple query */
				{
					const char *query_string;

					query_string = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					exec_simple_query(query_string);

					send_rfq = true;
				}
				break;

			case 'P':			/* parse */
				{
					const char *stmt_name;
					const char *query_string;
					int			numParams;
					Oid		   *paramTypes = NULL;

					stmt_name = pq_getmsgstring(&input_message);
					query_string = pq_getmsgstring(&input_message);
					numParams = pq_getmsgint(&input_message, 2);
					if (numParams > 0)
					{
						int			i;

						paramTypes = (Oid *) palloc(numParams * sizeof(Oid));
						for (i = 0; i < numParams; i++)
							paramTypes[i] = pq_getmsgint(&input_message, 4);
					}
					pq_getmsgend(&input_message);

					exec_parse_message(query_string, stmt_name,
									   paramTypes, numParams);
				}
				break;

			case 'B':			/* bind */

				/*
				 * this message is complex enough that it seems best to
				 * put the field extraction out-of-line
				 */
				exec_bind_message(&input_message);
				break;

			case 'E':			/* execute */
				{
					const char *portal_name;
					int			max_rows;

					portal_name = pq_getmsgstring(&input_message);
					max_rows = pq_getmsgint(&input_message, 4);
					pq_getmsgend(&input_message);

					exec_execute_message(portal_name, max_rows);
				}
				break;

			case 'F':			/* fastpath function call */
				/* Tell the collector what we're doing */
				pgstat_report_activity("<FASTPATH> function call");

				/* start an xact for this function invocation */
				start_xact_command();

				/* switch back to message context */
				MemoryContextSwitchTo(MessageContext);

				/* set snapshot in case function needs one */
				ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());

				if (HandleFunctionRequest(&input_message) == EOF)
				{
					/* lost frontend connection during F message input */

					/*
					 * Reset whereToSendOutput to prevent ereport from
					 * attempting to send any more messages to client.
					 */
					if (whereToSendOutput == Remote)
						whereToSendOutput = None;

					proc_exit(0);
				}

				/* commit the function-invocation transaction */
				finish_xact_command();

				send_rfq = true;
				break;

			case 'C':			/* close */
				{
					int			close_type;
					const char *close_target;

					close_type = pq_getmsgbyte(&input_message);
					close_target = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					switch (close_type)
					{
						case 'S':
							if (close_target[0] != '\0')
								DropPreparedStatement(close_target, false);
							else
							{
								/* special-case the unnamed statement */
								unnamed_stmt_pstmt = NULL;
								if (unnamed_stmt_context)
								{
									DropDependentPortals(unnamed_stmt_context);
									MemoryContextDelete(unnamed_stmt_context);
								}
								unnamed_stmt_context = NULL;
							}
							break;
						case 'P':
							{
								Portal		portal;

								portal = GetPortalByName(close_target);
								if (PortalIsValid(portal))
									PortalDrop(portal, false);
							}
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
							   errmsg("invalid CLOSE message subtype %d",
									  close_type)));
							break;
					}

					if (whereToSendOutput == Remote)
						pq_putemptymessage('3');		/* CloseComplete */
				}
				break;

			case 'D':			/* describe */
				{
					int			describe_type;
					const char *describe_target;

					describe_type = pq_getmsgbyte(&input_message);
					describe_target = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					switch (describe_type)
					{
						case 'S':
							exec_describe_statement_message(describe_target);
							break;
						case 'P':
							exec_describe_portal_message(describe_target);
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
							errmsg("invalid DESCRIBE message subtype %d",
								   describe_type)));
							break;
					}
				}
				break;

			case 'H':			/* flush */
				pq_getmsgend(&input_message);
				if (whereToSendOutput == Remote)
					pq_flush();
				break;

			case 'S':			/* sync */
				pq_getmsgend(&input_message);
				finish_xact_command();
				send_rfq = true;
				break;

				/*
				 * 'X' means that the frontend is closing down the socket.
				 * EOF means unexpected loss of frontend connection.
				 * Either way, perform normal shutdown.
				 */
			case 'X':
			case EOF:

				/*
				 * Reset whereToSendOutput to prevent ereport from
				 * attempting to send any more messages to client.
				 */
				if (whereToSendOutput == Remote)
					whereToSendOutput = None;

				/*
				 * NOTE: if you are tempted to add more code here, DON'T!
				 * Whatever you had in mind to do should be set up as an
				 * on_proc_exit or on_shmem_exit callback, instead.
				 * Otherwise it will fail to be called during other
				 * backend-shutdown scenarios.
				 */
				proc_exit(0);

			case 'd':			/* copy data */
			case 'c':			/* copy done */
			case 'f':			/* copy fail */

				/*
				 * Accept but ignore these messages, per protocol spec; we
				 * probably got here because a COPY failed, and the
				 * frontend is still sending data.
				 */
				break;

			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid frontend message type %d",
								firstchar)));
		}
	}							/* end of input-reading loop */

	/* can't get here because the above loop never exits */
	Assert(false);

	return 1;					/* keep compiler quiet */
}

#ifndef HAVE_GETRUSAGE
#include "rusagestub.h"
#else
#include <sys/resource.h>
#endif   /* HAVE_GETRUSAGE */

static struct rusage Save_r;
static struct timeval Save_t;

void
ResetUsage(void)
{
	getrusage(RUSAGE_SELF, &Save_r);
	gettimeofday(&Save_t, NULL);
	ResetBufferUsage();
	/* ResetTupleCount(); */
}

void
ShowUsage(const char *title)
{
	StringInfoData str;
	struct timeval user,
				sys;
	struct timeval elapse_t;
	struct rusage r;
	char	   *bufusage;

	getrusage(RUSAGE_SELF, &r);
	gettimeofday(&elapse_t, NULL);
	memcpy((char *) &user, (char *) &r.ru_utime, sizeof(user));
	memcpy((char *) &sys, (char *) &r.ru_stime, sizeof(sys));
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
	initStringInfo(&str);

	appendStringInfo(&str, "! system usage stats:\n");
	appendStringInfo(&str,
			"!\t%ld.%06ld elapsed %ld.%06ld user %ld.%06ld system sec\n",
					 (long) (elapse_t.tv_sec - Save_t.tv_sec),
					 (long) (elapse_t.tv_usec - Save_t.tv_usec),
					 (long) (r.ru_utime.tv_sec - Save_r.ru_utime.tv_sec),
				   (long) (r.ru_utime.tv_usec - Save_r.ru_utime.tv_usec),
					 (long) (r.ru_stime.tv_sec - Save_r.ru_stime.tv_sec),
				  (long) (r.ru_stime.tv_usec - Save_r.ru_stime.tv_usec));
	appendStringInfo(&str,
					 "!\t[%ld.%06ld user %ld.%06ld sys total]\n",
					 (long) user.tv_sec,
					 (long) user.tv_usec,
					 (long) sys.tv_sec,
					 (long) sys.tv_usec);
/* BeOS has rusage but only has some fields, and not these... */
#if defined(HAVE_GETRUSAGE)
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] filesystem blocks in/out\n",
					 r.ru_inblock - Save_r.ru_inblock,
	/* they only drink coffee at dec */
					 r.ru_oublock - Save_r.ru_oublock,
					 r.ru_inblock, r.ru_oublock);
	appendStringInfo(&str,
		  "!\t%ld/%ld [%ld/%ld] page faults/reclaims, %ld [%ld] swaps\n",
					 r.ru_majflt - Save_r.ru_majflt,
					 r.ru_minflt - Save_r.ru_minflt,
					 r.ru_majflt, r.ru_minflt,
					 r.ru_nswap - Save_r.ru_nswap,
					 r.ru_nswap);
	appendStringInfo(&str,
	 "!\t%ld [%ld] signals rcvd, %ld/%ld [%ld/%ld] messages rcvd/sent\n",
					 r.ru_nsignals - Save_r.ru_nsignals,
					 r.ru_nsignals,
					 r.ru_msgrcv - Save_r.ru_msgrcv,
					 r.ru_msgsnd - Save_r.ru_msgsnd,
					 r.ru_msgrcv, r.ru_msgsnd);
	appendStringInfo(&str,
		 "!\t%ld/%ld [%ld/%ld] voluntary/involuntary context switches\n",
					 r.ru_nvcsw - Save_r.ru_nvcsw,
					 r.ru_nivcsw - Save_r.ru_nivcsw,
					 r.ru_nvcsw, r.ru_nivcsw);
#endif   /* HAVE_GETRUSAGE */

	bufusage = ShowBufferUsage();
	appendStringInfo(&str, "! buffer usage stats:\n%s", bufusage);
	pfree(bufusage);

	/* remove trailing newline */
	if (str.data[str.len - 1] == '\n')
		str.data[--str.len] = '\0';

	ereport(LOG,
			(errmsg_internal("%s", title),
			 errdetail("%s", str.data)));

	pfree(str.data);
}

/*
 * on_proc_exit handler to log end of session
 */
static void
log_disconnections(int code, Datum arg)
{
	Port	   *port = MyProcPort;
	struct timeval end;
	int			hours,
				minutes,
				seconds;

	char		session_time[20];
	char		uname[6 + NAMEDATALEN];
	char		dbname[10 + NAMEDATALEN];
	char		remote_host[7 + NI_MAXHOST];
	char		remote_port[7 + NI_MAXSERV];

	snprintf(uname, sizeof(uname), " user=%s", port->user_name);
	snprintf(dbname, sizeof(dbname), " database=%s", port->database_name);
	snprintf(remote_host, sizeof(remote_host), " host=%s",
			 port->remote_host);
	snprintf(remote_port, sizeof(remote_port), " port=%s", port->remote_port);


	gettimeofday(&end, NULL);

	if (end.tv_usec < port->session_start.tv_usec)
	{
		end.tv_sec--;
		end.tv_usec += 1000000;
	}
	end.tv_sec -= port->session_start.tv_sec;
	end.tv_usec -= port->session_start.tv_usec;

	hours = end.tv_sec / 3600;
	end.tv_sec %= 3600;
	minutes = end.tv_sec / 60;
	seconds = end.tv_sec % 60;

	/* if time has gone backwards for some reason say so, or print time */

	if (end.tv_sec < 0)
		snprintf(session_time, sizeof(session_time), "negative!");
	else

		/*
		 * for stricter accuracy here we could round - this is close
		 * enough
		 */
		snprintf(session_time, sizeof(session_time),
				 "%d:%02d:%02d.%02d",
				 hours, minutes, seconds, (int) (end.tv_usec / 10000));

	ereport(
			LOG,
			(errmsg("disconnection: session time: %s%s%s%s%s",
				session_time, uname, dbname, remote_host, remote_port)));

}
