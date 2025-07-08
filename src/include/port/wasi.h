#pragma once

#ifndef I_WASI
#define I_WASI

#undef HAVE_PTHREAD

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


#define system(command) system_wasi(command)
extern int system_wasi(const char *command);

static pid_t
fork(void) {
    puts("# 34:" __FILE__ ": fork -1");
    return -1;
}

static int
pipe(int fd[2]) {
    puts("# 40: pipe");
    abort();
    return -1;
}

// ============= signal ==============

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




#include <unistd.h> // for uid_t, pid_t
static uid_t
getuid(void) {
    return 1000;
}


// WIP: semaphores
// https://github.com/bytecodealliance/wasm-micro-runtime/issues/3054
#include <sys/sem.h>

static int
semctl(int semid, int semnum, int cmd, ...) {
    return 0; // -1;
}

static int
semget(key_t key, int nsems, int semflg) {
#if 0 // PGDEBUG
    printf("# 120: semget(key_t key = %d, int nsems=%d, int semflg=%d)\n", key, nsems, semflg);
#endif
    return 1;
}

static int
semop(int semid, struct sembuf *sops, size_t nsops) {
    return 0;  // -1;
}

static int
dup(int fd) {
    puts("# 132: dup");
    return fd;
}

static int
dup2(int old, int new) {
    puts("# 138:" __FILE__ ": dup2");
    return -1;
}


// <sys/resource.h>

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

/*

static int
getrusage(int who, struct rusage *usage) {
    return -1;
}
*/

// STUBS
extern int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
extern int sigwait(const sigset_t *restrict set, int *restrict sig);
extern int sigismember(const sigset_t *set, int signum);
extern int sigpending(sigset_t *set);


// ****************************************************************************************
// ****************************************************************************************
// ****************************************************************************************
// ****************************************************************************************


#ifndef __wasi__p2


// ======== signal ========================



// ====================================================
// unistd
extern unsigned int alarm(unsigned int seconds);

// ====================================================

// WIP :

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
    fprintf(stderr, "# 186: shm_open(%s) => %d\n", tmpnam, fd);
    return fd;
}

static int
shm_unlink(const char *name) {
    char tmpnam[128];
    snprintf(tmpnam, 128, "/tmp%s", name);
    fprintf(stderr, "# 194: shm_unlink(%s) STUB\n", tmpnam);
    return remove(tmpnam); // -1
}

#endif


// time.h

static void
tzset(void) {
    puts("# 205: tzset(void) STUB");
}

#if defined(PG_INITDB) || defined(FE_UTILS_PRINT) || defined(PG_DUMP_PARALLEL)
static void
__SIG_IGN(int param) {
}
#endif


static int
listen(int sockfd, int backlog) {
    return 0;
}

static struct group *_Nullable
getgrnam(const char *_Nullable name) {
    return NULL;
}

#endif // __wasi__p2

#endif // I_WASI


