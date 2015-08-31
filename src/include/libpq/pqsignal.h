/*-------------------------------------------------------------------------
 *
 * pqsignal.h
 *	  Backend signal(2) support (see also src/port/pqsignal.c)
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pqsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQSIGNAL_H
#define PQSIGNAL_H

#include <signal.h>

#ifndef WIN32
extern sigset_t UnBlockSig,
			BlockSig,
			StartupBlockSig;

#define PG_SETMASK(mask)	sigprocmask(SIG_SETMASK, mask, NULL)
#else							/* WIN32 */
/*
 * Windows doesn't provide the POSIX signal API, so we use something
 * approximating the old BSD signal API.
 */
extern int	UnBlockSig,
			BlockSig,
			StartupBlockSig;

extern int	pqsigsetmask(int mask);

#define PG_SETMASK(mask)		pqsigsetmask(*(mask))
#define sigaddset(set, signum)	(*(set) |= (sigmask(signum)))
#define sigdelset(set, signum)	(*(set) &= ~(sigmask(signum)))
#endif   /* WIN32 */

extern void pqinitmask(void);

#endif   /* PQSIGNAL_H */
