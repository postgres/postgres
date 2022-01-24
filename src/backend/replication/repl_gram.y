%{
/*-------------------------------------------------------------------------
 *
 * repl_gram.y				- Parser for the replication commands
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/repl_gram.y
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlogdefs.h"
#include "nodes/makefuncs.h"
#include "nodes/replnodes.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"


/* Result of the parsing is returned here */
Node *replication_parse_result;


/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.  Note this only works with
 * bison >= 2.0.  However, in bison 1.875 the default is to use alloca()
 * if possible, so there's not really much problem anyhow, at least if
 * you're building with gcc.
 */
#define YYMALLOC palloc
#define YYFREE   pfree

%}

%expect 0
%name-prefix="replication_yy"

%union {
		char					*str;
		bool					boolval;
		uint32					uintval;

		XLogRecPtr				recptr;
		Node					*node;
		List					*list;
		DefElem					*defelt;
}

/* Non-keyword tokens */
%token <str> SCONST IDENT
%token <uintval> UCONST
%token <recptr> RECPTR

/* Keyword tokens. */
%token K_BASE_BACKUP
%token K_IDENTIFY_SYSTEM
%token K_SHOW
%token K_START_REPLICATION
%token K_CREATE_REPLICATION_SLOT
%token K_DROP_REPLICATION_SLOT
%token K_TIMELINE_HISTORY
%token K_LABEL
%token K_PROGRESS
%token K_FAST
%token K_WAIT
%token K_NOWAIT
%token K_MAX_RATE
%token K_WAL
%token K_TABLESPACE_MAP
%token K_NOVERIFY_CHECKSUMS
%token K_TIMELINE
%token K_PHYSICAL
%token K_LOGICAL
%token K_SLOT
%token K_RESERVE_WAL
%token K_TEMPORARY
%token K_EXPORT_SNAPSHOT
%token K_NOEXPORT_SNAPSHOT
%token K_USE_SNAPSHOT

%type <node>	command
%type <node>	base_backup start_replication start_logical_replication
				create_replication_slot drop_replication_slot identify_system
				timeline_history show
%type <list>	base_backup_opt_list
%type <defelt>	base_backup_opt
%type <uintval>	opt_timeline
%type <list>	plugin_options plugin_opt_list
%type <defelt>	plugin_opt_elem
%type <node>	plugin_opt_arg
%type <str>		opt_slot var_name
%type <boolval>	opt_temporary
%type <list>	create_slot_opt_list
%type <defelt>	create_slot_opt

%%

firstcmd: command opt_semicolon
				{
					replication_parse_result = $1;
				}
			;

opt_semicolon:	';'
				| /* EMPTY */
				;

command:
			identify_system
			| base_backup
			| start_replication
			| start_logical_replication
			| create_replication_slot
			| drop_replication_slot
			| timeline_history
			| show
			;

/*
 * IDENTIFY_SYSTEM
 */
identify_system:
			K_IDENTIFY_SYSTEM
				{
					$$ = (Node *) makeNode(IdentifySystemCmd);
				}
			;

/*
 * SHOW setting
 */
show:
			K_SHOW var_name
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);
					n->name = $2;
					$$ = (Node *) n;
				}

var_name:	IDENT	{ $$ = $1; }
			| var_name '.' IDENT
				{ $$ = psprintf("%s.%s", $1, $3); }
		;

/*
 * BASE_BACKUP [LABEL '<label>'] [PROGRESS] [FAST] [WAL] [NOWAIT]
 * [MAX_RATE %d] [TABLESPACE_MAP] [NOVERIFY_CHECKSUMS]
 */
base_backup:
			K_BASE_BACKUP base_backup_opt_list
				{
					BaseBackupCmd *cmd = makeNode(BaseBackupCmd);
					cmd->options = $2;
					$$ = (Node *) cmd;
				}
			;

base_backup_opt_list:
			base_backup_opt_list base_backup_opt
				{ $$ = lappend($1, $2); }
			| /* EMPTY */
				{ $$ = NIL; }
			;

base_backup_opt:
			K_LABEL SCONST
				{
				  $$ = makeDefElem("label",
								   (Node *)makeString($2), -1);
				}
			| K_PROGRESS
				{
				  $$ = makeDefElem("progress",
								   (Node *)makeInteger(true), -1);
				}
			| K_FAST
				{
				  $$ = makeDefElem("fast",
								   (Node *)makeInteger(true), -1);
				}
			| K_WAL
				{
				  $$ = makeDefElem("wal",
								   (Node *)makeInteger(true), -1);
				}
			| K_NOWAIT
				{
				  $$ = makeDefElem("nowait",
								   (Node *)makeInteger(true), -1);
				}
			| K_MAX_RATE UCONST
				{
				  $$ = makeDefElem("max_rate",
								   (Node *)makeInteger($2), -1);
				}
			| K_TABLESPACE_MAP
				{
				  $$ = makeDefElem("tablespace_map",
								   (Node *)makeInteger(true), -1);
				}
			| K_NOVERIFY_CHECKSUMS
				{
				  $$ = makeDefElem("noverify_checksums",
								   (Node *)makeInteger(true), -1);
				}
			;

create_replication_slot:
			/* CREATE_REPLICATION_SLOT slot TEMPORARY PHYSICAL RESERVE_WAL */
			K_CREATE_REPLICATION_SLOT IDENT opt_temporary K_PHYSICAL create_slot_opt_list
				{
					CreateReplicationSlotCmd *cmd;
					cmd = makeNode(CreateReplicationSlotCmd);
					cmd->kind = REPLICATION_KIND_PHYSICAL;
					cmd->slotname = $2;
					cmd->temporary = $3;
					cmd->options = $5;
					$$ = (Node *) cmd;
				}
			/* CREATE_REPLICATION_SLOT slot TEMPORARY LOGICAL plugin */
			| K_CREATE_REPLICATION_SLOT IDENT opt_temporary K_LOGICAL IDENT create_slot_opt_list
				{
					CreateReplicationSlotCmd *cmd;
					cmd = makeNode(CreateReplicationSlotCmd);
					cmd->kind = REPLICATION_KIND_LOGICAL;
					cmd->slotname = $2;
					cmd->temporary = $3;
					cmd->plugin = $5;
					cmd->options = $6;
					$$ = (Node *) cmd;
				}
			;

create_slot_opt_list:
			create_slot_opt_list create_slot_opt
				{ $$ = lappend($1, $2); }
			| /* EMPTY */
				{ $$ = NIL; }
			;

create_slot_opt:
			K_EXPORT_SNAPSHOT
				{
				  $$ = makeDefElem("export_snapshot",
								   (Node *)makeInteger(true), -1);
				}
			| K_NOEXPORT_SNAPSHOT
				{
				  $$ = makeDefElem("export_snapshot",
								   (Node *)makeInteger(false), -1);
				}
			| K_USE_SNAPSHOT
				{
				  $$ = makeDefElem("use_snapshot",
								   (Node *)makeInteger(true), -1);
				}
			| K_RESERVE_WAL
				{
				  $$ = makeDefElem("reserve_wal",
								   (Node *)makeInteger(true), -1);
				}
			;

/* DROP_REPLICATION_SLOT slot */
drop_replication_slot:
			K_DROP_REPLICATION_SLOT IDENT
				{
					DropReplicationSlotCmd *cmd;
					cmd = makeNode(DropReplicationSlotCmd);
					cmd->slotname = $2;
					cmd->wait = false;
					$$ = (Node *) cmd;
				}
			| K_DROP_REPLICATION_SLOT IDENT K_WAIT
				{
					DropReplicationSlotCmd *cmd;
					cmd = makeNode(DropReplicationSlotCmd);
					cmd->slotname = $2;
					cmd->wait = true;
					$$ = (Node *) cmd;
				}
			;

/*
 * START_REPLICATION [SLOT slot] [PHYSICAL] %X/%X [TIMELINE %d]
 */
start_replication:
			K_START_REPLICATION opt_slot opt_physical RECPTR opt_timeline
				{
					StartReplicationCmd *cmd;

					cmd = makeNode(StartReplicationCmd);
					cmd->kind = REPLICATION_KIND_PHYSICAL;
					cmd->slotname = $2;
					cmd->startpoint = $4;
					cmd->timeline = $5;
					$$ = (Node *) cmd;
				}
			;

/* START_REPLICATION SLOT slot LOGICAL %X/%X options */
start_logical_replication:
			K_START_REPLICATION K_SLOT IDENT K_LOGICAL RECPTR plugin_options
				{
					StartReplicationCmd *cmd;
					cmd = makeNode(StartReplicationCmd);
					cmd->kind = REPLICATION_KIND_LOGICAL;
					cmd->slotname = $3;
					cmd->startpoint = $5;
					cmd->options = $6;
					$$ = (Node *) cmd;
				}
			;
/*
 * TIMELINE_HISTORY %d
 */
timeline_history:
			K_TIMELINE_HISTORY UCONST
				{
					TimeLineHistoryCmd *cmd;

					if ($2 <= 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 (errmsg("invalid timeline %u", $2))));

					cmd = makeNode(TimeLineHistoryCmd);
					cmd->timeline = $2;

					$$ = (Node *) cmd;
				}
			;

opt_physical:
			K_PHYSICAL
			| /* EMPTY */
			;

opt_temporary:
			K_TEMPORARY						{ $$ = true; }
			| /* EMPTY */					{ $$ = false; }
			;

opt_slot:
			K_SLOT IDENT
				{ $$ = $2; }
			| /* EMPTY */
				{ $$ = NULL; }
			;

opt_timeline:
			K_TIMELINE UCONST
				{
					if ($2 <= 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 (errmsg("invalid timeline %u", $2))));
					$$ = $2;
				}
				| /* EMPTY */			{ $$ = 0; }
			;


plugin_options:
			'(' plugin_opt_list ')'			{ $$ = $2; }
			| /* EMPTY */					{ $$ = NIL; }
		;

plugin_opt_list:
			plugin_opt_elem
				{
					$$ = list_make1($1);
				}
			| plugin_opt_list ',' plugin_opt_elem
				{
					$$ = lappend($1, $3);
				}
		;

plugin_opt_elem:
			IDENT plugin_opt_arg
				{
					$$ = makeDefElem($1, $2, -1);
				}
		;

plugin_opt_arg:
			SCONST							{ $$ = (Node *) makeString($1); }
			| /* EMPTY */					{ $$ = NULL; }
		;

%%

#include "repl_scanner.c"
