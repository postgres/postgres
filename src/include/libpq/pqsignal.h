/*-------------------------------------------------------------------------
 *
 * pqsignal.h
 *	  prototypes for the reliable BSD-style signal(2) routine.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/libpq/pqsignal.h,v 1.24 2004/01/27 00:45:26 momjian Exp $
 *
 * NOTES
 *	  This shouldn't be in libpq, but the monitor and some other
 *	  things need it...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQSIGNAL_H
#define PQSIGNAL_H

#ifndef WIN32
#include <signal.h>
#endif

#ifndef WIN32
#define pqkill(pid,sig) kill(pid,sig)
#define pqsigsetmask(mask) sigsetmask(mask)
#else
int pqkill(int pid, int sig);
int pqsigsetmask(int mask);
#endif


#ifdef HAVE_SIGPROCMASK
extern sigset_t UnBlockSig,
			BlockSig,
			AuthBlockSig;

#define PG_SETMASK(mask)	sigprocmask(SIG_SETMASK, mask, NULL)
#else
extern int	UnBlockSig,
			BlockSig,
			AuthBlockSig;

#define PG_SETMASK(mask)	pqsigsetmask(*((int*)(mask)))
#endif

typedef void (*pqsigfunc) (int);

extern void pqinitmask(void);

extern pqsigfunc pqsignal(int signo, pqsigfunc func);

#ifdef WIN32
#define sigmask(sig) ( 1 << (sig-1) )

void pgwin32_signal_initialize(void);
extern HANDLE pgwin32_main_thread_handle;
#define PG_POLL_SIGNALS() WaitForSingleObjectEx(pgwin32_main_thread_handle,0,TRUE);

/* Define signal numbers. Override system values, since they are not
   complete anyway */

#undef SIGHUP
#define	SIGHUP	1	/* hangup */

#undef	SIGINT	
#define	SIGINT	2	/* interrupt */

#undef	SIGQUIT	
#define	SIGQUIT	3	/* quit */

#undef	SIGILL	
#define	SIGILL	4	/* illegal instruction (not reset when caught) */

#undef	SIGTRAP	
#define	SIGTRAP	5	/* trace trap (not reset when caught) */

#undef	SIGABRT	
#define	SIGABRT	6	/* abort(void) */

#undef	SIGIOT	
#define	SIGIOT	SIGABRT	/* compatibility */

#undef	SIGEMT	
#define	SIGEMT	7	/* EMT instruction */

#undef	SIGFPE	
#define	SIGFPE	8	/* floating point exception */

#undef	SIGKILL	
#define	SIGKILL	9	/* kill (cannot be caught or ignored) */

#undef	SIGBUS	
#define	SIGBUS	10	/* bus error */

#undef	SIGSEGV	
#define	SIGSEGV	11	/* segmentation violation */

#undef	SIGSYS	
#define	SIGSYS	12	/* non-existent system call invoked */

#undef	SIGSYS	
#define	SIGPIPE	13	/* write on a pipe with no one to read it */

#undef	SIGALRM	
#define	SIGALRM	14	/* alarm clock */

#undef	SIGTERM	
#define	SIGTERM	15	/* software termination signal from kill */

#undef	SIGURG	
#define	SIGURG	16	/* urgent condition on IO channel */

#undef	SIGSTOP	
#define	SIGSTOP	17	/* sendable stop signal not from tty */

#undef	SIGTSTP	
#define	SIGTSTP	18	/* stop signal from tty */

#undef	SIGCONT	
#define	SIGCONT	19	/* continue a stopped process */

#undef	SIGCHLD	
#define	SIGCHLD	20	/* to parent on child stop or exit */

#undef	SIGTTIN	
#define	SIGTTIN	21	/* to readers pgrp upon background tty read */

#undef	SIGTTOU	
#define	SIGTTOU	22	/* like TTIN for output if (tp->t_local&LTOSTOP) */

#undef	SIGIO	
#define	SIGIO	23	/* input/output possible signal */

#undef	SIGXCPU	
#define	SIGXCPU	24	/* exceeded CPU time limit */

#undef	SIGXFSZ	
#define	SIGXFSZ	25	/* exceeded file size limit */

#undef	SIGVTALR
#define	SIGVTALRM 26	/* virtual time alarm */

#undef	SIGPROF	
#define	SIGPROF	27	/* profiling time alarm */

#undef SIGWINCH 
#define SIGWINCH 28	/* window size changes */

#undef SIGINFO	
#define SIGINFO	29	/* information request */

#undef SIGUSR1 
#define SIGUSR1 30	/* user defined signal 1 */

#undef SIGUSR2 
#define SIGUSR2 31	/* user defined signal 2 */

#undef SIG_DFL
#undef SIG_ERR
#undef SIG_IGN
#define SIG_DFL ((pqsigfunc)0)
#define SIG_ERR ((pqsigfunc)-1)
#define SIG_IGN ((pqsigfunc)1)

#endif

#endif   /* PQSIGNAL_H */
