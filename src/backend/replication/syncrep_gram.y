%{
/*-------------------------------------------------------------------------
 *
 * syncrep_gram.y				- Parser for synchronous_standby_names
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/syncrep_gram.y
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "replication/syncrep.h"
#include "utils/formatting.h"

/* Result of the parsing is returned here */
SyncRepConfigData	*syncrep_parse_result;

static SyncRepConfigData *create_syncrep_config(char *num_sync, List *members);

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
%name-prefix="syncrep_yy"

%union
{
	char	   *str;
	List	   *list;
	SyncRepConfigData  *config;
}

%token <str> NAME NUM

%type <config> result standby_config
%type <list> standby_list
%type <str> standby_name

%start result

%%
result:
		standby_config				{ syncrep_parse_result = $1; }
;
standby_config:
		standby_list				{ $$ = create_syncrep_config("1", $1); }
		| NUM '(' standby_list ')'		{ $$ = create_syncrep_config($1, $3); }
;
standby_list:
		standby_name				{ $$ = list_make1($1);}
		| standby_list ',' standby_name		{ $$ = lappend($1, $3);}
;
standby_name:
		NAME					{ $$ = $1; }
		| NUM					{ $$ = $1; }
;
%%

static SyncRepConfigData *
create_syncrep_config(char *num_sync, List *members)
{
	SyncRepConfigData *config =
		(SyncRepConfigData *) palloc(sizeof(SyncRepConfigData));

	config->num_sync = atoi(num_sync);
	config->members = members;
	return config;
}

#include "syncrep_scanner.c"
