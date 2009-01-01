/*-------------------------------------------------------------------------
 *
 * scansup.h
 *	  scanner support routines.  used by both the bootstrap lexer
 * as well as the normal lexer
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/scansup.h,v 1.23 2009/01/01 17:24:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef SCANSUP_H
#define SCANSUP_H

extern char *scanstr(const char *s);

extern char *downcase_truncate_identifier(const char *ident, int len,
							 bool warn);

extern void truncate_identifier(char *ident, int len, bool warn);

extern bool scanner_isspace(char ch);

#endif   /* SCANSUP_H */
