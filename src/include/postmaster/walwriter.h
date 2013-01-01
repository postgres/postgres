/*-------------------------------------------------------------------------
 *
 * walwriter.h
 *	  Exports from postmaster/walwriter.c.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 *
 * src/include/postmaster/walwriter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALWRITER_H
#define _WALWRITER_H

/* GUC options */
extern int	WalWriterDelay;

extern void WalWriterMain(void) __attribute__((noreturn));

#endif   /* _WALWRITER_H */
