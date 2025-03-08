#ifndef I_WASI
#define I_WASI

#if defined(HAVE_SETSID)
#undef HAVE_SETSID
#endif

#if defined(HAVE_GETRLIMIT)
#undef HAVE_GETRLIMIT
#endif


#define PLATFORM_DEFAULT_SYNC_METHOD    SYNC_METHOD_FDATASYNC

#define EMSCRIPTEN_KEEPALIVE __attribute__((used))
#define __declspec( dllimport ) __attribute__((used))

#define em_callback_func void
#define emscripten_set_main_loop(...)
#define emscripten_force_exit(...)
#define EM_JS(...)

#include "/tmp/pglite/include/wasm_common.h"


static pid_t
fork(void) {
    puts("# 31: fork -1");
    return -1;
}


// TODO: socket here
// ==================================================================

#include <sys/socket.h>
static int
listen(int sockfd, int backlog) {
    return 0;
}

static struct group *_Nullable
getgrnam(const char *_Nullable name) {
    return NULL;
}

static int
getsockname(int sockfd, struct sockaddr *restrict addr, socklen_t *restrict addrlen) {
    return -1;
}

static int
getaddrinfo(const char *restrict node,
                   const char *restrict service,
                   void *restrict hints,
                   void **restrict res) {
    puts("# 60: getaddrinfo");
    return -1;
}
static void
freeaddrinfo(void *res) {
    puts("# 65: freeaddrinfo");
}

extern ssize_t recvfrom_bc(int socket, void *buffer, size_t length, int flags, void *address, socklen_t *address_len);

// ======== signal ========================
#define SA_RESTART 4
#define SIG_SETMASK   2

#define SIG_BLOCK     0
#define SIG_UNBLOCK   1

/* A signal handler.  */
typedef void (*handler_t) (int signal);
typedef unsigned char sigset_t;
typedef void (*__sighandler_t) (int);

struct sigaction {
    __sighandler_t sa_handler;
    unsigned long sa_flags;
#ifdef SA_RESTORER
    __sigrestore_t sa_restorer;
#endif
    sigset_t sa_mask;
};
extern int sigemptyset(sigset_t *set);
extern int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
extern int sigdelset (sigset_t *set, int sig);
extern int sigfillset (sigset_t *set);
extern int sigprocmask (int operation, const sigset_t *set, sigset_t *old_set);
extern int sigaddset (sigset_t *set, int sig);

// STUBS
extern int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
extern int sigismember(const sigset_t *set, int signum);
extern int sigpending(sigset_t *set);
extern int sigwait(const sigset_t *restrict set, int *restrict sig);

// ====================================================
// unistd
extern unsigned int alarm(unsigned int seconds);

// ====================================================


#include <setjmp.h>

static int
sigsetjmp(sigjmp_buf env, int savesigs) {
//    puts("# 120: sigsetjmp");
    return 0;
}

static void
siglongjmp(sigjmp_buf env, int val) {
    puts("# 120: siglongjmp");
}



// WIP :

#include <unistd.h>
static uid_t
getuid(void) {
    return 1000;
}

static int
dup(int fd) {
    puts("# 128: dup");
    return fd;
}
static int
dup2(int old, int new) {
    puts("# 140: dup2");
    return -1;
}
static int
pipe(int fd[2]) {
    puts("# 145: pipe");
    abort();
    return -1;
}

#include <sys/resource.h>
#define RLIMIT_NOFILE 7
#define RLIMIT_STACK 3
#define RLIM_INFINITY ((unsigned long int)(~0UL))

struct rlimit {
    unsigned long   rlim_cur;
    unsigned long   rlim_max;
};
static int
getrlimit(int resource, struct rlimit *rlim) {
    return -1;
}


static const char *gai_strerror_msg = "# 165: gai_strerror_msg";
static const char *
gai_strerror(int errcode) {
    return gai_strerror_msg;
}


static int
getrusage(int who, struct rusage *usage) {
    return -1;
}

// WIP: semaphores here
// ==================================================================
#include <sys/sem.h>

static int
semctl(int semid, int semnum, int cmd, ...) {
    return 0; // -1;
}

static int
semget(key_t key, int nsems, int semflg) {
#if 0 // PGDEBUG
    printf("# 213: semget(key_t key = %d, int nsems=%d, int semflg=%d)\n", key, nsems, semflg);
#endif
    return 1;
}

static int
semop(int semid, struct sembuf *sops, size_t nsops) {
    return 0;  // -1;
}



#include <sys/mman.h>
#if defined(PYDK)
extern int shm_open(const char *name, int oflag, mode_t mode);
extern int shm_unlink(const char *name);
#else
static int
shm_open(const char *name, int oflag, mode_t mode) {
    char tmpnam[128];
    int fd;
    snprintf(tmpnam, 128, "/tmp%s", name);
    fd=fileno(fopen(tmpnam, "w+"));
    fprintf(stderr, "# 212: shm_open(%s) => %d\n", tmpnam, fd);
    return fd;
}

static int
shm_unlink(const char *name) {
    char tmpnam[128];
    snprintf(tmpnam, 128, "/tmp%s", name);
    fprintf(stderr, "# 220: shm_unlink(%s) STUB\n", tmpnam);
    return remove(tmpnam); // -1
}

#endif

// initdb chmod
#if defined(__wasi__)
    #define chmod(...) 0
#endif



#define system(command) system_wasi(command)
extern int system_wasi(const char *command);



// time.h

static void
tzset(void) {
    puts("# 241: tzset(void) STUB");
}

#if defined(PG_INITDB) || defined(FE_UTILS_PRINT) || defined(PG_DUMP_PARALLEL)
static void
__SIG_IGN(int param) {
}
#endif


extern void sock_flush();


#endif // I_WASI

#ifndef I_WASI
#define I_WASI

#if defined(HAVE_SETSID)
#undef HAVE_SETSID
#endif

#if defined(HAVE_GETRLIMIT)
#undef HAVE_GETRLIMIT
#endif


#define PLATFORM_DEFAULT_SYNC_METHOD    SYNC_METHOD_FDATASYNC

#define EMSCRIPTEN_KEEPALIVE __attribute__((used))
#define __declspec( dllimport ) __attribute__((used))

#define em_callback_func void
#define emscripten_set_main_loop(...)
#define emscripten_force_exit(...)
#define EM_JS(...)

#include "/tmp/pglite/include/wasm_common.h"


static pid_t
fork(void) {
    puts("# 31: fork -1");
    return -1;
}


// TODO: socket here
// ==================================================================

#include <sys/socket.h>
static int
listen(int sockfd, int backlog) {
    return 0;
}

static struct group *_Nullable
getgrnam(const char *_Nullable name) {
    return NULL;
}

static int
getsockname(int sockfd, struct sockaddr *restrict addr, socklen_t *restrict addrlen) {
    return -1;
}

static int
getaddrinfo(const char *restrict node,
                   const char *restrict service,
                   void *restrict hints,
                   void **restrict res) {
    puts("# 60: getaddrinfo");
    return -1;
}
static void
freeaddrinfo(void *res) {
    puts("# 65: freeaddrinfo");
}

extern ssize_t recvfrom_bc(int socket, void *buffer, size_t length, int flags, void *address, socklen_t *address_len);

// ======== signal ========================
#define SA_RESTART 4
#define SIG_SETMASK   2

#define SIG_BLOCK     0
#define SIG_UNBLOCK   1

/* A signal handler.  */
typedef void (*handler_t) (int signal);
typedef unsigned char sigset_t;
typedef void (*__sighandler_t) (int);

struct sigaction {
    __sighandler_t sa_handler;
    unsigned long sa_flags;
#ifdef SA_RESTORER
    __sigrestore_t sa_restorer;
#endif
    sigset_t sa_mask;
};
extern int sigemptyset(sigset_t *set);
extern int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
extern int sigdelset (sigset_t *set, int sig);
extern int sigfillset (sigset_t *set);
extern int sigprocmask (int operation, const sigset_t *set, sigset_t *old_set);
extern int sigaddset (sigset_t *set, int sig);

// STUBS
extern int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
extern int sigismember(const sigset_t *set, int signum);
extern int sigpending(sigset_t *set);
extern int sigwait(const sigset_t *restrict set, int *restrict sig);

// ====================================================
// unistd
extern unsigned int alarm(unsigned int seconds);

// ====================================================


#include <setjmp.h>

static int
sigsetjmp(sigjmp_buf env, int savesigs) {
//    puts("# 120: sigsetjmp");
    return 0;
}

static void
siglongjmp(sigjmp_buf env, int val) {
    puts("# 120: siglongjmp");
}



// WIP :

#include <unistd.h>
static uid_t
getuid(void) {
    return 1000;
}

static int
dup(int fd) {
    puts("# 128: dup");
    return fd;
}
static int
dup2(int old, int new) {
    puts("# 140: dup2");
    return -1;
}
static int
pipe(int fd[2]) {
    puts("# 145: pipe");
    abort();
    return -1;
}

#include <sys/resource.h>
#define RLIMIT_NOFILE 7
#define RLIMIT_STACK 3
#define RLIM_INFINITY ((unsigned long int)(~0UL))

struct rlimit {
    unsigned long   rlim_cur;
    unsigned long   rlim_max;
};
static int
getrlimit(int resource, struct rlimit *rlim) {
    return -1;
}


static const char *gai_strerror_msg = "# 165: gai_strerror_msg";
static const char *
gai_strerror(int errcode) {
    return gai_strerror_msg;
}


static int
getrusage(int who, struct rusage *usage) {
    return -1;
}

// WIP: semaphores here
// ==================================================================
#include <sys/sem.h>

static int
semctl(int semid, int semnum, int cmd, ...) {
    return 0; // -1;
}

static int
semget(key_t key, int nsems, int semflg) {
#if 0 // PGDEBUG
    printf("# 213: semget(key_t key = %d, int nsems=%d, int semflg=%d)\n", key, nsems, semflg);
#endif
    return 1;
}

static int
semop(int semid, struct sembuf *sops, size_t nsops) {
    return 0;  // -1;
}



#include <sys/mman.h>
#if defined(PYDK)
extern int shm_open(const char *name, int oflag, mode_t mode);
extern int shm_unlink(const char *name);
#else
static int
shm_open(const char *name, int oflag, mode_t mode) {
    char tmpnam[128];
    int fd;
    snprintf(tmpnam, 128, "/tmp%s", name);
    fd=fileno(fopen(tmpnam, "w+"));
    fprintf(stderr, "# 212: shm_open(%s) => %d\n", tmpnam, fd);
    return fd;
}

static int
shm_unlink(const char *name) {
    char tmpnam[128];
    snprintf(tmpnam, 128, "/tmp%s", name);
    fprintf(stderr, "# 220: shm_unlink(%s) STUB\n", tmpnam);
    return remove(tmpnam); // -1
}

#endif



#define system(command) system_wasi(command)
extern int system_wasi(const char *command);



// time.h

static void
tzset(void) {
    puts("# 241: tzset(void) STUB");
}

#if defined(PG_INITDB) || defined(FE_UTILS_PRINT) || defined(PG_DUMP_PARALLEL)
static void
__SIG_IGN(int param) {
}
#endif


extern void sock_flush();


#endif // I_WASI

