/*
 * timeline.h
 *
 * Functions for reading and writing timeline history files.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/timeline.h
 */
#ifndef TIMELINE_H
#define TIMELINE_H

#include "access/xlogdefs.h"
#include "nodes/pg_list.h"

extern List *readTimeLineHistory(TimeLineID targetTLI);
extern bool existsTimeLineHistory(TimeLineID probeTLI);
extern TimeLineID findNewestTimeLine(TimeLineID startTLI);
extern void writeTimeLineHistory(TimeLineID newTLI, TimeLineID parentTLI,
					 TimeLineID endTLI, XLogSegNo endLogSegNo, char *reason);

#endif   /* TIMELINE_H */
