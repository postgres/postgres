/*-------------------------------------------------------------------------
 *
 * cmdtag.c
 *	  Data and routines for commandtag names and enumeration.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/tcop/cmdtag.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "tcop/cmdtag.h"
#include "utils/builtins.h"


typedef struct CommandTagBehavior
{
	const char *name;			/* tag name, e.g. "SELECT" */
	const uint8 namelen;		/* set to strlen(name) */
	const bool	event_trigger_ok;
	const bool	table_rewrite_ok;
	const bool	display_rowcount;	/* should the number of rows affected be
									 * shown in the command completion string */
} CommandTagBehavior;

#define PG_CMDTAG(tag, name, evtrgok, rwrok, rowcnt) \
	{ name, (uint8) (sizeof(name) - 1), evtrgok, rwrok, rowcnt },

static const CommandTagBehavior tag_behavior[] = {
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

const char *
GetCommandTagNameAndLen(CommandTag commandTag, Size *len)
{
	*len = (Size) tag_behavior[commandTag].namelen;
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

/*
 * BuildQueryCompletionString
 *		Build a string containing the command tag name with the
 *		QueryCompletion's nprocessed for command tags with display_rowcount
 *		set.  Returns the strlen of the constructed string.
 *
 * The caller must ensure that buff is at least COMPLETION_TAG_BUFSIZE bytes.
 *
 * If nameonly is true, then the constructed string will contain only the tag
 * name.
 */
Size
BuildQueryCompletionString(char *buff, const QueryCompletion *qc,
						   bool nameonly)
{
	CommandTag	tag = qc->commandTag;
	Size		taglen;
	const char *tagname = GetCommandTagNameAndLen(tag, &taglen);
	char	   *bufp;

	/*
	 * We assume the tagname is plain ASCII and therefore requires no encoding
	 * conversion.
	 */
	memcpy(buff, tagname, taglen);
	bufp = buff + taglen;

	/* ensure that the tagname isn't long enough to overrun the buffer */
	Assert(taglen <= COMPLETION_TAG_BUFSIZE - MAXINT8LEN - 4);

	/*
	 * In PostgreSQL versions 11 and earlier, it was possible to create a
	 * table WITH OIDS.  When inserting into such a table, INSERT used to
	 * include the Oid of the inserted record in the completion tag.  To
	 * maintain compatibility in the wire protocol, we now write a "0" (for
	 * InvalidOid) in the location where we once wrote the new record's Oid.
	 */
	if (command_tag_display_rowcount(tag) && !nameonly)
	{
		if (tag == CMDTAG_INSERT)
		{
			*bufp++ = ' ';
			*bufp++ = '0';
		}
		*bufp++ = ' ';
		bufp += pg_ulltoa_n(qc->nprocessed, bufp);
	}

	/* and finally, NUL terminate the string */
	*bufp = '\0';

	Assert((bufp - buff) == strlen(buff));

	return bufp - buff;
}
