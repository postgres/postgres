/*
 * timeline.h
 *
 * Functions for reading and writing timeline history files.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/timeline.h
 */
#ifndef TIMELINE_H
#define TIMELINE_H

#include "access/xlogdefs.h"
#include "nodes/pg_list.h"

/*
 * A list of these structs describes the timeline history of the server. Each
 * TimeLineHistoryEntry represents a piece of WAL belonging to the history,
 * from newest to oldest. All WAL positions between 'begin' and 'end' belong to
 * the timeline represented by the entry. Together the 'begin' and 'end'
 * pointers of all the entries form a contiguous line from beginning of time
 * to infinity.
 */
typedef struct
{
	TimeLineID	tli;
	XLogRecPtr	begin;			/* inclusive */
	XLogRecPtr	end;			/* exclusive, 0 means infinity */
} TimeLineHistoryEntry;

extern List *readTimeLineHistory(TimeLineID targetTLI);
extern bool existsTimeLineHistory(TimeLineID probeTLI);
extern TimeLineID findNewestTimeLine(TimeLineID startTLI);
extern void writeTimeLineHistory(TimeLineID newTLI, TimeLineID parentTLI,
					 XLogRecPtr switchpoint, char *reason);
extern void writeTimeLineHistoryFile(TimeLineID tli, char *content, int size);
extern void restoreTimeLineHistoryFiles(TimeLineID begin, TimeLineID end);
extern bool tliInHistory(TimeLineID tli, List *expectedTLIs);
extern TimeLineID tliOfPointInHistory(XLogRecPtr ptr, List *history);
extern XLogRecPtr tliSwitchPoint(TimeLineID tli, List *history,
			   TimeLineID *nextTLI);

#endif   /* TIMELINE_H */
