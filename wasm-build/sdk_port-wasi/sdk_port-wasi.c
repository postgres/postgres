
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
  *set = ((2U << (NSIG - 1)) - 1) & ~ SIGABRT_COMPAT_MASK;
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
                  abort ();
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

int
pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    return 0;
}

int sigpending(sigset_t *set) {
    return -1;
}

int sigwait(const sigset_t *restrict set, int *restrict sig) {
    return 0;
}

unsigned int alarm(unsigned int seconds) {
    return 0;
}


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
extern FILE* SOCKET_FILE;
extern int SOCKET_DATA;
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


FILE *tmpfile(void) {
    return fopen(mktemp("/tmp/tmpfile"),"w");
}





// unix socket via file emulation using sched_yiedl for event pump.
// =================================================================================================

#include <sched.h>

volatile int stage = 0;
volatile int fd_queue = 0;
volatile int fd_out=2;
volatile FILE *fd_FILE = NULL;

// default fd is stderr
int socket(int domain, int type, int protocol) {
#if 0
    printf("# 404 : domain =%d type=%d proto=%d -> FORCE FD to 3 \n", domain , type, protocol);
    return 3;
#else
    printf("# 408 : domain =%d type=%d proto=%d\n", domain , type, protocol);
#endif
    if (domain|AF_UNIX) {
        fd_FILE = fopen(PGS_ILOCK, "w");
        if (fd_FILE) {
            fd_out = fileno(fd_FILE);
            printf("# 414: AF_UNIX sock=%d (fd_sock write) FILE=%s\n", fd_out, PGS_ILOCK);
        } else {
            printf("# 416: AF_UNIX ERROR OPEN (w/w+) FILE=%s\n", PGS_ILOCK);
            abort();
        }
    }
    return fd_out;
}

int connect(int socket, void *address, socklen_t address_len) {
#if 1
    puts("# 425: connect STUB");
    //fd_out = 3;
    return 0;
#else
    puts("# 429: connect EINPROGRESS");
    errno = EINPROGRESS;
    return -1;
#endif
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, void *dest_addr, socklen_t addrlen) {
    int sent = write( fd_out, buf, len);

    printf("# 438: send/sendto(%d ?= %ld )/%zu sockfd=%d fno=%d fd_out=%d)\n", sent, ftell(fd_FILE), len, sockfd, fileno(fd_FILE), fd_out);
    fd_queue+=sent;
    return sent;
}

ssize_t sdk_send(int sockfd, const void *buf, size_t len, int flags) {
    return sendto(sockfd, buf, len, flags, NULL, 0);
}

volatile bool web_warned = false;

void sock_flush() {
    if (fd_queue) {
        printf(" -- 451 sockflush : AIO YIELD, expecting %s filled on return --\n", PGS_OUT);
        if (!fd_FILE) {
            if (!web_warned) {
                puts("# 454: WARNING: fd_FILE not set but queue not empty, assuming web");
                web_warned = true;
            }

         } else {
            printf("#       459: SENT=%ld/%d fd_out=%d fno=%d\n", ftell(fd_FILE), fd_queue, fd_out, fileno(fd_FILE));
            fclose(fd_FILE);
            rename(PGS_ILOCK, PGS_IN);
//freopen(PGS_ILOCK, "w+", fd_FILE);
            fd_FILE = fopen(PGS_ILOCK, "w");
            fd_out = fileno(fd_FILE);
            printf("#       465: fd_out=%d fno=%d\n", fd_out, fileno(fd_FILE));
        }
        fd_queue = 0;
        sched_yield();
        return;
    }

    printf(" -- 472 sockflush[%d] : NO YIELD --\n",stage);

    // limit inf loops
    if (stage++ > 1024) {
        puts("# 476 sock_flush : busy looping ?");
        abort();
    }
}


volatile int fd_current_pos = 0;
volatile int fd_filesize = 0;


ssize_t recvfrom_bc(int socket, void *buffer, size_t length, int flags, void *address, socklen_t *address_len) {
//    int busy = 0;
    int rcv = -1;
    sock_flush();
/*
    while (access(PGS_OUT, F_OK) != 0) {
        if (!(++busy % 555111)) {
            printf("# 471: FIXME: busy wait (%d) for input stream %s\n", busy, PGS_OUT);
        }
        if (busy>1665334) {
            errno = EINTR;
            return -1;
        }
    }
*/
    FILE *sock_in = fopen(PGS_OUT,"r");
    if (sock_in) {
        if (!fd_filesize) {
            fseek(sock_in, 0L, SEEK_END);
            fd_filesize = ftell(sock_in);
        }
        fseek(sock_in, fd_current_pos, SEEK_SET);

        char *buf = buffer;
        buf[0] = 0;
        rcv = fread(buf, 1, length, sock_in);

        if (rcv<fd_filesize) {
            fd_current_pos = ftell(sock_in);
            if (fd_current_pos<fd_filesize) {
                printf("# 506: recvfrom_bc(%s max=%zu) block=%d read=%d / %d\n", PGS_OUT, length, rcv, fd_current_pos, fd_filesize);
                fclose(sock_in);
                return rcv;
            }
        }

        // fully read
        printf("# 513: recvfrom_bc(%s max=%zu total=%d) read=%d\n", PGS_OUT, length, fd_filesize, rcv);
        fd_queue = 0;
        fd_filesize = 0;
        fd_current_pos = 0;
        fclose(sock_in);
        unlink(PGS_OUT);

    } else {
        printf("# 529: recvfrom_bc(%s max=%d) ERROR\n", PGS_OUT, length);
        errno = EINTR;
    }
    return rcv;

}


ssize_t recvfrom(int socket, void *buffer, size_t length, int flags, void *address, socklen_t *address_len) {
    int busy = 0;
    int rcv = -1;
    sock_flush();

/*
    while (access(PGS_OUT, F_OK) != 0) {
        if (!(++busy % 555111)) {
            printf("# 471: FIXME: busy wait (%d) for input stream %s\n", busy, PGS_OUT);
        }
        if (busy>1665334) {
            errno = EINTR;
            return -1;
        }
    }
*/
    FILE *sock_in = fopen(PGS_OUT,"r");
    if (sock_in) {
        if (!fd_filesize) {
            fseek(sock_in, 0L, SEEK_END);
            fd_filesize = ftell(sock_in);
        }
        fseek(sock_in, fd_current_pos, SEEK_SET);

        char *buf = buffer;
        buf[0] = 0;
        rcv = fread(buf, 1, length, sock_in);

        if (rcv<fd_filesize) {
            fd_current_pos = ftell(sock_in);
            if (fd_current_pos<fd_filesize) {
                printf("# 568: recvfrom(%s max=%d) block=%d read=%d / %d\n", PGS_OUT, length, rcv, fd_current_pos, fd_filesize);
                fclose(sock_in);
                return rcv;
            }
        }

        // fully read
        printf("# 575: recvfrom(%s max=%d total=%d) read=%d\n", PGS_OUT, length, fd_filesize, rcv);
        fd_queue = 0;
        fd_filesize = 0;
        fd_current_pos = 0;
        fclose(sock_in);
        unlink(PGS_OUT);

    } else {
        printf("# 583: recvfrom(%s max=%d) ERROR\n", PGS_OUT, length);
        errno = EINTR;
    }
    return rcv;
}


ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return recvfrom(sockfd, buf, len, flags, NULL, NULL);
}


pid_t waitpid(pid_t pid, int *status, int options) {
    fputs(__FILE__ ": pid_t waitpid(pid_t pid, int *status, int options) STUB", stderr);
    return -1;
}

pid_t sdk_getpid(void) {
    return (pid_t)42;
}

