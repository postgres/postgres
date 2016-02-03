/*-------------------------------------------------------------------------
 *
 * fsmfuncs.c
 *	  Functions to investigate FSM pages
 *
 * These functions are restricted to superusers for the fear of introducing
 * security holes if the input checking isn't as water-tight as it should.
 * You'd need to be superuser to obtain a raw page image anyway, so
 * there's hardly any use case for using these without superuser-rights
 * anyway.
 *
 * Copyright (c) 2007-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pageinspect/fsmfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/fsm_internals.h"
#include "utils/builtins.h"

/*
 * Dumps the contents of a FSM page.
 */
PG_FUNCTION_INFO_V1(fsm_page_contents);

Datum
fsm_page_contents(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	StringInfoData sinfo;
	FSMPage		fsmpage;
	int			i;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use raw page functions"))));

	fsmpage = (FSMPage) PageGetContents(VARDATA(raw_page));

	initStringInfo(&sinfo);

	for (i = 0; i < NodesPerPage; i++)
	{
		if (fsmpage->fp_nodes[i] != 0)
			appendStringInfo(&sinfo, "%d: %d\n", i, fsmpage->fp_nodes[i]);
	}
	appendStringInfo(&sinfo, "fp_next_slot: %d\n", fsmpage->fp_next_slot);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(sinfo.data, sinfo.len));
}
