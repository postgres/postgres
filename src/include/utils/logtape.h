/*-------------------------------------------------------------------------
 *
 * logtape.h
 *	  Management of "logical tapes" within temporary files.
 *
 * See logtape.c for explanations.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: logtape.h,v 1.9 2003/08/04 02:40:15 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef LOGTAPE_H
#define LOGTAPE_H

/* LogicalTapeSet is an opaque type whose details are not known outside logtape.c. */

typedef struct LogicalTapeSet LogicalTapeSet;

/*
 * prototypes for functions in logtape.c
 */

extern LogicalTapeSet *LogicalTapeSetCreate(int ntapes);
extern void LogicalTapeSetClose(LogicalTapeSet *lts);
extern size_t LogicalTapeRead(LogicalTapeSet *lts, int tapenum,
				void *ptr, size_t size);
extern void LogicalTapeWrite(LogicalTapeSet *lts, int tapenum,
				 void *ptr, size_t size);
extern void LogicalTapeRewind(LogicalTapeSet *lts, int tapenum, bool forWrite);
extern void LogicalTapeFreeze(LogicalTapeSet *lts, int tapenum);
extern bool LogicalTapeBackspace(LogicalTapeSet *lts, int tapenum,
					 size_t size);
extern bool LogicalTapeSeek(LogicalTapeSet *lts, int tapenum,
				long blocknum, int offset);
extern void LogicalTapeTell(LogicalTapeSet *lts, int tapenum,
				long *blocknum, int *offset);

#endif   /* LOGTAPE_H */
