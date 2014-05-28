%{
/*-------------------------------------------------------------------------
 *
 * repl_gram.y				- Parser for the replication commands
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
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
%token K_START_REPLICATION
%token K_CREATE_REPLICATION_SLOT
%token K_DROP_REPLICATION_SLOT
%token K_TIMELINE_HISTORY
%token K_LABEL
%token K_PROGRESS
%token K_FAST
%token K_NOWAIT
%token K_MAX_RATE
%token K_WAL
%token K_TIMELINE
%token K_PHYSICAL
%token K_LOGICAL
%token K_SLOT

%type <node>	command
%type <node>	base_backup start_replication start_logical_replication create_replication_slot drop_replication_slot identify_system timeline_history
%type <list>	base_backup_opt_list
%type <defelt>	base_backup_opt
%type <uintval>	opt_timeline
%type <list>	plugin_options plugin_opt_list
%type <defelt>	plugin_opt_elem
%type <node>	plugin_opt_arg
%type <str>		opt_slot

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
 * BASE_BACKUP [LABEL '<label>'] [PROGRESS] [FAST] [WAL] [NOWAIT] [MAX_RATE %d]
 */
base_backup:
			K_BASE_BACKUP base_backup_opt_list
				{
					BaseBackupCmd *cmd = (BaseBackupCmd *) makeNode(BaseBackupCmd);
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
								   (Node *)makeString($2));
				}
			| K_PROGRESS
				{
				  $$ = makeDefElem("progress",
								   (Node *)makeInteger(TRUE));
				}
			| K_FAST
				{
				  $$ = makeDefElem("fast",
								   (Node *)makeInteger(TRUE));
				}
			| K_WAL
				{
				  $$ = makeDefElem("wal",
								   (Node *)makeInteger(TRUE));
				}
			| K_NOWAIT
				{
				  $$ = makeDefElem("nowait",
								   (Node *)makeInteger(TRUE));
				}
			| K_MAX_RATE UCONST
				{
				  $$ = makeDefElem("max_rate",
								   (Node *)makeInteger($2));
				}
			;

create_replication_slot:
			/* CREATE_REPLICATION_SLOT slot PHYSICAL */
			K_CREATE_REPLICATION_SLOT IDENT K_PHYSICAL
				{
					CreateReplicationSlotCmd *cmd;
					cmd = makeNode(CreateReplicationSlotCmd);
					cmd->kind = REPLICATION_KIND_PHYSICAL;
					cmd->slotname = $2;
					$$ = (Node *) cmd;
				}
			/* CREATE_REPLICATION_SLOT slot LOGICAL plugin */
			| K_CREATE_REPLICATION_SLOT IDENT K_LOGICAL IDENT
				{
					CreateReplicationSlotCmd *cmd;
					cmd = makeNode(CreateReplicationSlotCmd);
					cmd->kind = REPLICATION_KIND_LOGICAL;
					cmd->slotname = $2;
					cmd->plugin = $4;
					$$ = (Node *) cmd;
				}
			;

/* DROP_REPLICATION_SLOT slot */
drop_replication_slot:
			K_DROP_REPLICATION_SLOT IDENT
				{
					DropReplicationSlotCmd *cmd;
					cmd = makeNode(DropReplicationSlotCmd);
					cmd->slotname = $2;
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
					cmd->kind = REPLICATION_KIND_LOGICAL;;
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
					$$ = makeDefElem($1, $2);
				}
		;

plugin_opt_arg:
			SCONST							{ $$ = (Node *) makeString($1); }
			| /* EMPTY */					{ $$ = NULL; }
		;
%%

#include "repl_scanner.c"
