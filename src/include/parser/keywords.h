/*-------------------------------------------------------------------------
 *
 * keywords.h
 *	  lexical token lookup for reserved words in postgres SQL
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: keywords.h,v 1.8 2000/08/29 02:00:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef KEYWORDS_H
#define KEYWORDS_H

typedef struct ScanKeyword
{
	char	   *name;
	int			value;
} ScanKeyword;

extern ScanKeyword *ScanKeywordLookup(char *text);

#endif	 /* KEYWORDS_H */
