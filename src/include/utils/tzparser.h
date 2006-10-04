/*-------------------------------------------------------------------------
 *
 * tzparser.h
 *	  Timezone offset file parsing definitions.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/tzparser.h,v 1.2 2006/10/04 00:30:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TZPARSER_H
#define TZPARSER_H

/*
 * The result of parsing a timezone configuration file is an array of
 * these structs, in order by abbrev.  We export this because datetime.c
 * needs it.
 */
typedef struct tzEntry
{
	/* the actual data: TZ abbrev (downcased), offset, DST flag */
	char	   *abbrev;
	int			offset;			/* in seconds from UTC */
	bool		is_dst;
	/* source information (for error messages) */
	int			lineno;
	const char *filename;
} tzEntry;


extern bool load_tzoffsets(const char *filename, bool doit, int elevel);

#endif   /* TZPARSER_H */
