/*-------------------------------------------------------------------------
 *
 * pqsignal.h
 *	  Backend signal(2) support (see also src/port/pqsignal.c)
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pqsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQSIGNAL_H
#define PQSIGNAL_H

#include <signal.h>

#define PG_SETMASK(mask)	sigprocmask(SIG_SETMASK, mask, NULL)

#ifdef WIN32
/* Emulate POSIX sigset_t APIs on Windows */
typedef int sigset_t;

extern int	pqsigprocmask(int how, const sigset_t *set, sigset_t *oset);

#define SIG_BLOCK				1
#define SIG_UNBLOCK				2
#define SIG_SETMASK				3
#define sigprocmask(how, set, oset) pqsigprocmask((how), (set), (oset))
#define sigemptyset(set)		(*(set) = 0)
#define sigfillset(set)			(*(set) = ~0)
#define sigaddset(set, signum)	(*(set) |= (sigmask(signum)))
#define sigdelset(set, signum)	(*(set) &= ~(sigmask(signum)))
#endif							/* WIN32 */

extern PGDLLIMPORT sigset_t UnBlockSig;
extern PGDLLIMPORT sigset_t BlockSig;
extern PGDLLIMPORT sigset_t StartupBlockSig;

extern void pqinitmask(void);

/* pqsigfunc is declared in src/include/port.h */
extern pqsigfunc pqsignal_pm(int signo, pqsigfunc func);

#endif							/* PQSIGNAL_H */
