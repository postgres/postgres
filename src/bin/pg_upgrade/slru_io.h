/*
 * slru_io.h
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/slru_io.h
 */

#ifndef SLRU_IO_H
#define SLRU_IO_H

/*
 * State for reading or writing an SLRU, with a one page buffer.
 */
typedef struct SlruSegState
{
	bool		writing;
	bool		long_segment_names;

	char	   *dir;
	char	   *fn;
	int			fd;
	int64		segno;
	uint64		pageno;

	PGAlignedBlock buf;
} SlruSegState;

extern SlruSegState *AllocSlruRead(const char *dir, bool long_segment_names);
extern char *SlruReadSwitchPageSlow(SlruSegState *state, uint64 pageno);
extern void FreeSlruRead(SlruSegState *state);

static inline char *
SlruReadSwitchPage(SlruSegState *state, uint64 pageno)
{
	if (state->segno != -1 && pageno == state->pageno)
		return state->buf.data;
	return SlruReadSwitchPageSlow(state, pageno);
}

extern SlruSegState *AllocSlruWrite(const char *dir, bool long_segment_names);
extern char *SlruWriteSwitchPageSlow(SlruSegState *state, uint64 pageno);
extern void FreeSlruWrite(SlruSegState *state);

static inline char *
SlruWriteSwitchPage(SlruSegState *state, uint64 pageno)
{
	if (state->segno != -1 && pageno == state->pageno)
		return state->buf.data;
	return SlruWriteSwitchPageSlow(state, pageno);
}

#endif							/* SLRU_IO_H */
