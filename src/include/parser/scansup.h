/*-------------------------------------------------------------------------
 *
 * scansup.h
 *	  scanner support routines.  used by both the bootstrap lexer
 * as well as the normal lexer
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: scansup.h,v 1.13.4.1 2004/02/21 00:35:13 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef SCANSUP_H
#define SCANSUP_H

extern char *scanstr(const char *s);

extern char *downcase_truncate_identifier(const char *ident, int len,
										  bool warn);

extern void truncate_identifier(char *ident, int len, bool warn);

#endif   /* SCANSUP_H */
