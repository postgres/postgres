/*-------------------------------------------------------------------------
 *
 * walwriter.h
 *	  Exports from postmaster/walwriter.c.
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/postmaster/walwriter.h,v 1.1 2007/07/24 04:54:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALWRITER_H
#define _WALWRITER_H

/* GUC options */
extern int	WalWriterDelay;

extern void WalWriterMain(void);

#endif   /* _WALWRITER_H */
