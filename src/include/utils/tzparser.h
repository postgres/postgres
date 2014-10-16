/*-------------------------------------------------------------------------
 *
 * tzparser.h
 *	  Timezone offset file parsing definitions.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/tzparser.h,v 1.6 2010/01/02 16:58:10 momjian Exp $
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
	/* the actual data */
	char	   *abbrev;			/* TZ abbreviation (downcased) */
	char	   *zone;			/* zone name if dynamic abbrev, else NULL */
	/* for a dynamic abbreviation, offset/is_dst are not used */
	int			offset;			/* offset in seconds from UTC */
	bool		is_dst;			/* true if a DST abbreviation */
	/* source information (for error messages) */
	int			lineno;
	const char *filename;
} tzEntry;


extern bool load_tzoffsets(const char *filename, bool doit, int elevel);

#endif   /* TZPARSER_H */
