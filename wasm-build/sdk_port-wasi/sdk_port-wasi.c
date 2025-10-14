#include <stdlib.h> // for abort
// WIP: signal here
// ==================================================================
#include <signal.h>

/* Convenience type when working with signal handlers.  */
typedef void (*sa_handler_t) (int);

/* Return the handler of a signal, as a sa_handler_t value regardless
   of its true type.  The resulting function can be compared to
   special values like SIG_IGN but it is not portable to call it.  */
// static inline sa_handler_t;

/*
struct sigaction {
    __sighandler_t sa_handler;
    unsigned long sa_flags;
#ifdef SA_RESTORER
    __sigrestore_t sa_restorer;
#endif
    sigset_t sa_mask;
};
*/

#ifdef SIGABRT_COMPAT
#define SIGABRT_COMPAT_MASK (1U << SIGABRT_COMPAT)
#else
#define SIGABRT_COMPAT_MASK 0
#endif


/* Present to allow compilation, but unsupported by gnulib.  */
#if 0
union sigval
{
  int sival_int;
  void *sival_ptr;
};


struct siginfo_t
{
  int si_signo;
  int si_code;
  int si_errno;
  pid_t si_pid;
  uid_t si_uid;
  void *si_addr;
  int si_status;
  long si_band;
  union sigval si_value;
};

typedef struct siginfo_t siginfo_t;

struct sigaction_x
{
  union
  {
    void (*_sa_handler) (int);
    /* Present to allow compilation, but unsupported by gnulib.  POSIX
       says that implementations may, but not must, make sa_sigaction
       overlap with sa_handler, but we know of no implementation where
       they do not overlap.  */
    void (*_sa_sigaction) (int, siginfo_t *, void *);
  } _sa_func;
  sigset_t sa_mask;
  /* Not all POSIX flags are supported.  */
  int sa_flags;
};
#endif // 0

#define sa_handler _sa_func._sa_handler
#define sa_sigaction _sa_func._sa_sigaction

/* Unsupported flags are not present.  */
#define SA_RESETHAND 1
#define SA_NODEFER 2
#define SA_RESTART 4

#define SIG_BLOCK     0
#define SIG_UNBLOCK   1



/* Set of current actions.  If sa_handler for an entry is NULL, then
   that signal is not currently handled by the sigaction handler.  */
struct sigaction volatile action_array[NSIG] /* = 0 */;

// typedef void (*__sighandler_t) (int);

# define _SIGSET_NWORDS        (1024 / (8 * sizeof (unsigned long int)))


/* Set of currently blocked signals.  */
volatile sigset_t blocked_set /* = 0 */;

/* Set of currently blocked and pending signals.  */
volatile sig_atomic_t pending_array[NSIG] /* = { 0 } */;

/* The previous signal handlers.
   Only the array elements corresponding to blocked signals are relevant.  */
volatile handler_t old_handlers[NSIG];


int
sigemptyset(sigset_t *set) {
  *set = 0;
  return 0;
}

int
sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    puts("# 96: sigaction STUB");
    return 0;
}

int
sigfillset (sigset_t *set) {
    // *set = (sigset_t)( ((2U << (NSIG - 1)) - 1) & ~ SIGABRT_COMPAT_MASK );
    if (set == NULL) {
        return -1;
    }
    *set = ~(sigset_t)0;
    return 0;
}

int
sigaddset (sigset_t *set, int sig) {
  if (sig >= 0 && sig < NSIG)
    {
      #ifdef SIGABRT_COMPAT
      if (sig == SIGABRT_COMPAT)
        sig = SIGABRT;
      #endif

      *set |= 1U << sig;
      return 0;
    }
  else
    {
      errno = EINVAL;
      return -1;
    }
}

int
sigdelset (sigset_t *set, int sig) {
  if (sig >= 0 && sig < NSIG)
    {
      #ifdef SIGABRT_COMPAT
      if (sig == SIGABRT_COMPAT)
        sig = SIGABRT;
      #endif

      *set &= ~(1U << sig);
      return 0;
    }
  else
    {
      errno = EINVAL;
      return -1;
    }
}

/* Signal handler that is installed for blocked signals.  */
void
blocked_handler (int sig)
{
  /* Reinstall the handler, in case the signal occurs multiple times
     while blocked.  There is an inherent race where an asynchronous
     signal in between when the kernel uninstalled the handler and
     when we reinstall it will trigger the default handler; oh
     well.  */
  signal (sig, blocked_handler);
  if (sig >= 0 && sig < NSIG)
    pending_array[sig] = 1;
}

int
sigprocmask (int operation, const sigset_t *set, sigset_t *old_set) {
  if (old_set != NULL)
    *old_set = blocked_set;

  if (set != NULL)
    {
      sigset_t new_blocked_set;
      sigset_t to_unblock;
      sigset_t to_block;

      switch (operation)
        {
        case SIG_BLOCK:
          new_blocked_set = blocked_set | *set;
          break;
        case SIG_SETMASK:
          new_blocked_set = *set;
          break;
        case SIG_UNBLOCK:
          new_blocked_set = blocked_set & ~*set;
          break;
        default:
          errno = EINVAL;
          return -1;
        }
      to_unblock = blocked_set & ~new_blocked_set;
      to_block = new_blocked_set & ~blocked_set;

      if (to_block != 0)
        {
          int sig;

          for (sig = 0; sig < NSIG; sig++)
            if ((to_block >> sig) & 1)
              {
                pending_array[sig] = 0;
                if ((old_handlers[sig] = signal (sig, blocked_handler)) != SIG_ERR)
                  blocked_set |= 1U << sig;
              }
        }

      if (to_unblock != 0)
        {
          sig_atomic_t received[NSIG];
          int sig;

          for (sig = 0; sig < NSIG; sig++)
            if ((to_unblock >> sig) & 1)
              {
                if (signal (sig, old_handlers[sig]) != blocked_handler)
                  /* The application changed a signal handler while the signal
                     was blocked, bypassing our rpl_signal replacement.
                     We don't support this.  */
                  abort();
                received[sig] = pending_array[sig];
                blocked_set &= ~(1U << sig);
                pending_array[sig] = 0;
              }
            else
              received[sig] = 0;

          for (sig = 0; sig < NSIG; sig++)
            if (received[sig])
              raise (sig);
        }
    }
  return 0;
}


// STUBS
int sigismember(const sigset_t *set, int signum) {
    return -1;
}



int sigwait(const sigset_t *restrict set, int *restrict sig) {
    return 0;
}

unsigned int alarm(unsigned int seconds) {
    return 0;
}

/*
int
pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    return 0;
}


int sigpending(sigset_t *set) {
    return -1;
}
*/

// WIP : shm
// ========================================================================================
volatile int shm_index = 0;

#include <sys/mman.h>
void get_shm_path(char *tmpnam, const char *name) {
    const char *shm = getenv("SHM");
    if (shm) {
        printf("# 281 SHM=%s.%d", shm, shm_index);
        snprintf(tmpnam, 128, "%s.%d", shm, shm_index++);
    } else {
        snprintf(tmpnam, 128, "/tmp%s", name);
    }
}

int shm_open(const char *name, int oflag, mode_t mode) {
    char tmpnam[128];
    int fd;
    get_shm_path(&tmpnam, name);
    fd=fileno(fopen(tmpnam, "w+"));
    fprintf(stderr, "# 287: shm_open(%s) => %d\n", tmpnam, fd);
    return fd;
}

int shm_unlink(const char *name) {
    char tmpnam[128];
    if (getenv("SHM")) {
        fprintf(stderr, "# 294: shm_unlink(%s) STUB\n", name);
        return 0;
    }
    get_shm_path(&tmpnam, name);
    return remove(tmpnam);
}


// popen
// ========================================================================================


#include <stdio.h> // FILE+fprintf
extern FILE* IDB_PIPE_FP;
extern int IDB_STAGE;

static inline int
ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix)
        return 0;
    size_t lenstr = strlen(str);
    size_t lensuffix = strlen(suffix);
    if (lensuffix >  lenstr)
        return 0;
    return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

int
system_wasi(const char *command) {
    fprintf(stderr, "# 164: system('%s')\n", command);
    return -1;
}

// pthread.h
// ========================================================================================


/*
int pthread_create(pthread_t *restrict thread,
                          const pthread_attr_t *restrict attr,
                          void *(*start_routine)(void *),
                          void *restrict arg) {
    puts("# 327: pthread_create STUB");
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    return 0;
}



int sdk_pthread_mutex_lock(void *mutex) {
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    return 0;
}

*/


void wait();

// present in share/wasi-sysroot/lib/wasm32-wasi/libwasi-emulated-signal.a(signal.o)
// void __SIG_IGN(int param) { }


pid_t waitpid(pid_t pid, int *status, int options) {
    fputs(__FILE__ ": pid_t waitpid(pid_t pid, int *status, int options) STUB", stderr);
    return -1;
}





// *********************************************************************************************
// *********************************************************************************************
// *********************************************************************************************
// *********************************************************************************************




#ifndef __wasi__p2


volatile FILE *fd_FILE = NULL;
volatile int fd_sock = 0;
volatile int fd_out = 2;
volatile int fd_queue = 0;

#endif // __wasi__p2
