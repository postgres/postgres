/*-------------------------------------------------------------------------
 *
 * cmdtag.c
 *	  Data and routines for commandtag names and enumeration.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/tcop/cmdtag.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "tcop/cmdtag.h"


typedef struct CommandTagBehavior
{
	const char *name;
	const bool	event_trigger_ok;
	const bool	table_rewrite_ok;
	const bool	display_rowcount;
} CommandTagBehavior;

#define PG_CMDTAG(tag, name, evtrgok, rwrok, rowcnt) \
	{ name, evtrgok, rwrok, rowcnt },

const CommandTagBehavior tag_behavior[COMMAND_TAG_NEXTTAG] = {
#include "tcop/cmdtaglist.h"
};

#undef PG_CMDTAG

void
InitializeQueryCompletion(QueryCompletion *qc)
{
	qc->commandTag = CMDTAG_UNKNOWN;
	qc->nprocessed = 0;
}

const char *
GetCommandTagName(CommandTag commandTag)
{
	return tag_behavior[commandTag].name;
}

bool
command_tag_display_rowcount(CommandTag commandTag)
{
	return tag_behavior[commandTag].display_rowcount;
}

bool
command_tag_event_trigger_ok(CommandTag commandTag)
{
	return tag_behavior[commandTag].event_trigger_ok;
}

bool
command_tag_table_rewrite_ok(CommandTag commandTag)
{
	return tag_behavior[commandTag].table_rewrite_ok;
}

/*
 * Search CommandTag by name
 *
 * Returns CommandTag, or CMDTAG_UNKNOWN if not recognized
 */
CommandTag
GetCommandTagEnum(const char *commandname)
{
	const CommandTagBehavior *base,
			   *last,
			   *position;
	int			result;

	if (commandname == NULL || *commandname == '\0')
		return CMDTAG_UNKNOWN;

	base = tag_behavior;
	last = tag_behavior + lengthof(tag_behavior) - 1;
	while (last >= base)
	{
		position = base + ((last - base) >> 1);
		result = pg_strcasecmp(commandname, position->name);
		if (result == 0)
			return (CommandTag) (position - tag_behavior);
		else if (result < 0)
			last = position - 1;
		else
			base = position + 1;
	}
	return CMDTAG_UNKNOWN;
}
