/*-------------------------------------------------------------------------
 *
 * pqsignal.h--
 *    prototypes for the reliable BSD-style signal(2) routine.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pqsignal.h,v 1.2 1996/10/31 09:49:01 scrappy Exp $
 *
 * NOTES
 *    This shouldn't be in libpq, but the monitor and some other
 *    things need it...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQSIGNAL_H
#define PQSIGNAL_H

#include <signal.h>


typedef void (*pqsigfunc)(int);

extern pqsigfunc pqsignal(int signo, pqsigfunc func);

#if defined(USE_POSIX_SIGNALS)
#define	signal(signo, handler)	pqsignal(signo, (pqsigfunc)(handler))
#endif /* USE_POSIX_SIGNALS */

#endif	/* PQSIGNAL_H */
