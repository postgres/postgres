/*-------------------------------------------------------------------------
 *
 * pqsignal.c
 *	  reliable BSD-style signal(2) routine stolen from RWW who stole it
 *	  from Stevens...
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/libpq/pqsignal.c,v 1.29 2004/01/27 00:45:26 momjian Exp $
 *
 * NOTES
 *		This shouldn't be in libpq, but the monitor and some other
 *		things need it...
 *
 *	A NOTE ABOUT SIGNAL HANDLING ACROSS THE VARIOUS PLATFORMS.
 *
 *	pg_config.h defines the macro HAVE_POSIX_SIGNALS for some platforms and
 *	not for others.  This file and pqsignal.h use that macro to decide
 *	how to handle signalling.
 *
 *	signal(2) handling - this is here because it affects some of
 *	the frontend commands as well as the backend server.
 *
 *	Ultrix and SunOS provide BSD signal(2) semantics by default.
 *
 *	SVID2 and POSIX signal(2) semantics differ from BSD signal(2)
 *	semantics.	We can use the POSIX sigaction(2) on systems that
 *	allow us to request restartable signals (SA_RESTART).
 *
 *	Some systems don't allow restartable signals at all unless we
 *	link to a special BSD library.
 *
 *	We devoutly hope that there aren't any systems that provide
 *	neither POSIX signals nor BSD signals.	The alternative
 *	is to do signal-handler reinstallation, which doesn't work well
 *	at all.
 * ------------------------------------------------------------------------*/
#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0400
#endif

#include "postgres.h"

#ifndef WIN32
#include <signal.h>
#else
#include <windows.h>
#endif

#include "libpq/pqsignal.h"


/*
 * Initialize BlockSig, UnBlockSig, and AuthBlockSig.
 *
 * BlockSig is the set of signals to block when we are trying to block
 * signals.  This includes all signals we normally expect to get, but NOT
 * signals that should never be turned off.
 *
 * AuthBlockSig is the set of signals to block during authentication;
 * it's essentially BlockSig minus SIGTERM, SIGQUIT, SIGALRM.
 *
 * UnBlockSig is the set of signals to block when we don't want to block
 * signals (is this ever nonzero??)
 */
void
pqinitmask(void)
{
#ifdef HAVE_SIGPROCMASK
	sigemptyset(&UnBlockSig);
	sigfillset(&BlockSig);
	sigfillset(&AuthBlockSig);

	/*
	 * Unmark those signals that should never be blocked. Some of these
	 * signal names don't exist on all platforms.  Most do, but might as
	 * well ifdef them all for consistency...
	 */
#ifdef SIGTRAP
	sigdelset(&BlockSig, SIGTRAP);
	sigdelset(&AuthBlockSig, SIGTRAP);
#endif
#ifdef SIGABRT
	sigdelset(&BlockSig, SIGABRT);
	sigdelset(&AuthBlockSig, SIGABRT);
#endif
#ifdef SIGILL
	sigdelset(&BlockSig, SIGILL);
	sigdelset(&AuthBlockSig, SIGILL);
#endif
#ifdef SIGFPE
	sigdelset(&BlockSig, SIGFPE);
	sigdelset(&AuthBlockSig, SIGFPE);
#endif
#ifdef SIGSEGV
	sigdelset(&BlockSig, SIGSEGV);
	sigdelset(&AuthBlockSig, SIGSEGV);
#endif
#ifdef SIGBUS
	sigdelset(&BlockSig, SIGBUS);
	sigdelset(&AuthBlockSig, SIGBUS);
#endif
#ifdef SIGSYS
	sigdelset(&BlockSig, SIGSYS);
	sigdelset(&AuthBlockSig, SIGSYS);
#endif
#ifdef SIGCONT
	sigdelset(&BlockSig, SIGCONT);
	sigdelset(&AuthBlockSig, SIGCONT);
#endif
#ifdef SIGTERM
	sigdelset(&AuthBlockSig, SIGTERM);
#endif
#ifdef SIGQUIT
	sigdelset(&AuthBlockSig, SIGQUIT);
#endif
#ifdef SIGALRM
	sigdelset(&AuthBlockSig, SIGALRM);
#endif
#else
	UnBlockSig = 0;
	BlockSig = sigmask(SIGHUP) | sigmask(SIGQUIT) |
		sigmask(SIGTERM) | sigmask(SIGALRM) |
		sigmask(SIGINT) | sigmask(SIGUSR1) |
		sigmask(SIGUSR2) | sigmask(SIGCHLD) |
		sigmask(SIGWINCH) | sigmask(SIGFPE);
	AuthBlockSig = sigmask(SIGHUP) |
		sigmask(SIGINT) | sigmask(SIGUSR1) |
		sigmask(SIGUSR2) | sigmask(SIGCHLD) |
		sigmask(SIGWINCH) | sigmask(SIGFPE);
#endif
}


#ifndef WIN32
/*
 * Set up a signal handler
 */
pqsigfunc
pqsignal(int signo, pqsigfunc func)
{
#if !defined(HAVE_POSIX_SIGNALS)
	return signal(signo, func);
#else
	struct sigaction act,
				oact;

	act.sa_handler = func;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	if (signo != SIGALRM)
		act.sa_flags |= SA_RESTART;
	if (sigaction(signo, &act, &oact) < 0)
		return SIG_ERR;
	return oact.sa_handler;
#endif   /* !HAVE_POSIX_SIGNALS */
}


#else


/* Win32 specific signals code */

/* pg_signal_crit_sec is used to protect only pg_signal_queue. That is the only
 * variable that can be accessed from the signal sending threads! */
static CRITICAL_SECTION pg_signal_crit_sec;
static int pg_signal_queue;

#define PG_SIGNAL_COUNT 32
static pqsigfunc pg_signal_array[PG_SIGNAL_COUNT];
static pqsigfunc pg_signal_defaults[PG_SIGNAL_COUNT];
static int pg_signal_mask;

HANDLE pgwin32_main_thread_handle;

/* Signal handling thread function */
static DWORD WINAPI pg_signal_thread(LPVOID param);

/* Initialization */
void pgwin32_signal_initialize(void) {
  int i;
  HANDLE signal_thread_handle;
  InitializeCriticalSection(&pg_signal_crit_sec);
  
  for (i = 0; i < PG_SIGNAL_COUNT; i++) {
    pg_signal_array[i] = SIG_DFL;
    pg_signal_defaults[i] = SIG_IGN;
  }
  pg_signal_mask = 0;
  pg_signal_queue = 0;
  
  /* Get handle to main thread so we can post calls to it later */
  if (!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),
		       GetCurrentProcess(),&pgwin32_main_thread_handle,
		       0,FALSE,DUPLICATE_SAME_ACCESS)) {
    fprintf(stderr,gettext("Failed to get main thread handle!\n"));
    exit(1);
  }
  
  /* Create thread for handling signals */
  signal_thread_handle = CreateThread(NULL,0,pg_signal_thread,NULL,0,NULL);
  if (signal_thread_handle == NULL) {
    fprintf(stderr,gettext("Failed to create signal handler thread!\n"));
    exit(1);
  }
}


/* Dispatch all signals currently queued and not blocked 
 * Blocked signals are ignored, and will be fired at the time of
 * the sigsetmask() call. */
static void dispatch_queued_signals(void) {
  int i;
  
  EnterCriticalSection(&pg_signal_crit_sec);
  while (pg_signal_queue & ~pg_signal_mask) {
    /* One or more unblocked signals queued for execution */
    
    int exec_mask = pg_signal_queue & ~pg_signal_mask;
    
    for (i = 0; i < PG_SIGNAL_COUNT; i++) {
      if (exec_mask & sigmask(i)) {
	/* Execute this signal */
	pqsigfunc sig = pg_signal_array[i];
	if (sig == SIG_DFL)
	  sig = pg_signal_defaults[i];
	pg_signal_queue &= ~sigmask(i);
	if (sig != SIG_ERR && sig != SIG_IGN &&	sig != SIG_DFL) {
	  LeaveCriticalSection(&pg_signal_crit_sec);
	  sig(i);
	  EnterCriticalSection(&pg_signal_crit_sec);
	  break; /* Restart outer loop, in case signal mask or queue
		    has been modified inside signal handler */
	}
      }
    }
  }
  LeaveCriticalSection(&pg_signal_crit_sec);
}

/* signal masking. Only called on main thread, no sync required */
int pqsigsetmask(int mask) {
  int prevmask;
  prevmask = pg_signal_mask;
  pg_signal_mask = mask;
  
  /* Dispatch any signals queued up right away, in case we have
     unblocked one or more signals previously queued */
  dispatch_queued_signals();
  
  return prevmask;
}


/* signal manipulation. Only called on main thread, no sync required */
pqsigfunc pqsignal(int signum, pqsigfunc handler) {
  pqsigfunc prevfunc;
  if (signum >= PG_SIGNAL_COUNT || signum < 0)
    return SIG_ERR;
  prevfunc = pg_signal_array[signum];
  pg_signal_array[signum] = handler;
  return prevfunc;
}

/* signal sending */
int pqkill(int pid, int sig) {
  char pipename[128];
  BYTE sigData = sig;
  BYTE sigRet = 0;
  DWORD bytes;
  
  if (sig >= PG_SIGNAL_COUNT || sig <= 0) {
    errno = EINVAL;
    return -1;
  }
  if (pid <= 0) {
    /* No support for process groups */
    errno = EINVAL;
    return -1;
  }
  wsprintf(pipename,"\\\\.\\pipe\\pgsignal_%i",pid);
  if (!CallNamedPipe(pipename,&sigData,1,&sigRet,1,&bytes,1000)) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND)
      errno = ESRCH;
    else if (GetLastError() == ERROR_ACCESS_DENIED)
      errno = EPERM;
    else
      errno = EINVAL;
    return -1;
  }
  if (bytes != 1 || sigRet != sig) {
    errno = ESRCH;
    return -1;
  }
  
  return 0;
}

/* APC callback scheduled on main thread when signals are fired */
static void CALLBACK pg_signal_apc(ULONG_PTR param) {
  dispatch_queued_signals();
}

/*
 * All functions below execute on the signal handler thread
 * and must be synchronized as such!
 * NOTE! The only global variable that can be used is
 * pg_signal_queue!
 */


static void pg_queue_signal(int signum) {
  if (signum >= PG_SIGNAL_COUNT || signum < 0)
    return;
  
  EnterCriticalSection(&pg_signal_crit_sec);
  pg_signal_queue |= sigmask(signum);
  LeaveCriticalSection(&pg_signal_crit_sec);
  
  QueueUserAPC(pg_signal_apc,pgwin32_main_thread_handle,(ULONG_PTR)NULL);
}

/* Signal dispatching thread */
static DWORD WINAPI pg_signal_dispatch_thread(LPVOID param) {
  HANDLE pipe = (HANDLE)param;
  BYTE sigNum;
  DWORD bytes;
  
  if (!ReadFile(pipe,&sigNum,1,&bytes,NULL)) {
    /* Client died before sending */
    CloseHandle(pipe);
    return 0;
  }
  if (bytes != 1) {
    /* Received <bytes> bytes over signal pipe (should be 1) */
    CloseHandle(pipe);
    return 0;
  }
  WriteFile(pipe,&sigNum,1,&bytes,NULL); /* Don't care if it works or not.. */
  FlushFileBuffers(pipe);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
  
  pg_queue_signal(sigNum);
  return 0;
}

/* Signal handling thread */
static DWORD WINAPI pg_signal_thread(LPVOID param) {
  char pipename[128];
  HANDLE pipe = INVALID_HANDLE_VALUE;
  
  wsprintf(pipename,"\\\\.\\pipe\\pgsignal_%i",GetCurrentProcessId());
  
  for (;;) {
    BOOL fConnected;
    HANDLE hThread;
    
    pipe = CreateNamedPipe(pipename,PIPE_ACCESS_DUPLEX,
			   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
			   PIPE_UNLIMITED_INSTANCES,16,16,1000,NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
      fprintf(stderr,gettext("Failed to create signal listener pipe: %i. Retrying.\n"),(int)GetLastError());
      SleepEx(500,TRUE);
      continue;
    }
    
    fConnected = ConnectNamedPipe(pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (fConnected) {
      hThread = CreateThread(NULL, 0, 
			     (LPTHREAD_START_ROUTINE)pg_signal_dispatch_thread,
			     (LPVOID)pipe,0,NULL);
      if (hThread == INVALID_HANDLE_VALUE) {
	fprintf(stderr,gettext("Failed to create signal dispatch thread: %i\n"),(int)GetLastError());
      }
      else 
	CloseHandle(hThread);
    }
    else
      /* Connection failed. Cleanup and try again */
      CloseHandle(pipe);
  }
  return 0;
}


#endif
