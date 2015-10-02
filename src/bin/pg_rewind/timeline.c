/*-------------------------------------------------------------------------
 *
 * timeline.c
 *	  timeline-related functions.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "pg_rewind.h"

#include "access/timeline.h"
#include "access/xlog_internal.h"

/*
 * This is copy-pasted from the backend readTimeLineHistory, modified to
 * return a malloc'd array and to work without backend functions.
 */
/*
 * Try to read a timeline's history file.
 *
 * If successful, return the list of component TLIs (the given TLI followed by
 * its ancestor TLIs).  If we can't find the history file, assume that the
 * timeline has no parents, and return a list of just the specified timeline
 * ID.
 */
TimeLineHistoryEntry *
rewind_parseTimeLineHistory(char *buffer, TimeLineID targetTLI, int *nentries)
{
	char	   *fline;
	TimeLineHistoryEntry *entry;
	TimeLineHistoryEntry *entries = NULL;
	int			nlines = 0;
	TimeLineID	lasttli = 0;
	XLogRecPtr	prevend;
	char	   *bufptr;
	bool		lastline = false;

	/*
	 * Parse the file...
	 */
	prevend = InvalidXLogRecPtr;
	bufptr = buffer;
	while (!lastline)
	{
		char	   *ptr;
		TimeLineID	tli;
		uint32		switchpoint_hi;
		uint32		switchpoint_lo;
		int			nfields;

		fline = bufptr;
		while (*bufptr && *bufptr != '\n')
			bufptr++;
		if (!(*bufptr))
			lastline = true;
		else
			*bufptr++ = '\0';

		/* skip leading whitespace and check for # comment */
		for (ptr = fline; *ptr; ptr++)
		{
			if (!isspace((unsigned char) *ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		nfields = sscanf(fline, "%u\t%X/%X", &tli, &switchpoint_hi, &switchpoint_lo);

		if (nfields < 1)
		{
			/* expect a numeric timeline ID as first field of line */
			fprintf(stderr, _("syntax error in history file: %s\n"), fline);
			fprintf(stderr, _("Expected a numeric timeline ID.\n"));
			exit(1);
		}
		if (nfields != 3)
		{
			fprintf(stderr, _("syntax error in history file: %s\n"), fline);
			fprintf(stderr, _("Expected a transaction log switchpoint location.\n"));
			exit(1);
		}
		if (entries && tli <= lasttli)
		{
			fprintf(stderr, _("invalid data in history file: %s\n"), fline);
			fprintf(stderr, _("Timeline IDs must be in increasing sequence.\n"));
			exit(1);
		}

		lasttli = tli;

		nlines++;
		entries = pg_realloc(entries, nlines * sizeof(TimeLineHistoryEntry));

		entry = &entries[nlines - 1];
		entry->tli = tli;
		entry->begin = prevend;
		entry->end = ((uint64) (switchpoint_hi)) << 32 | (uint64) switchpoint_lo;
		prevend = entry->end;

		/* we ignore the remainder of each line */
	}

	if (entries && targetTLI <= lasttli)
	{
		fprintf(stderr, _("invalid data in history file\n"));
		fprintf(stderr, _("Timeline IDs must be less than child timeline's ID.\n"));
		exit(1);
	}

	/*
	 * Create one more entry for the "tip" of the timeline, which has no entry
	 * in the history file.
	 */
	nlines++;
	if (entries)
		entries = pg_realloc(entries, nlines * sizeof(TimeLineHistoryEntry));
	else
		entries = pg_malloc(1 * sizeof(TimeLineHistoryEntry));

	entry = &entries[nlines - 1];
	entry->tli = targetTLI;
	entry->begin = prevend;
	entry->end = InvalidXLogRecPtr;

	*nentries = nlines;
	return entries;
}
