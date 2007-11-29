/* $PostgreSQL: pgsql/src/include/port/win32.h,v 1.63.2.3 2007/11/29 16:44:26 mha Exp $ */

#if defined(_MSC_VER) || defined(__BORLANDC__)
#define WIN32_ONLY_COMPILER
#endif

/* undefine and redefine after #include */
#undef mkdir

#undef ERROR
#define _WINSOCKAPI_
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#undef small
#include <process.h>
#include <signal.h>
#include <errno.h>
#include <direct.h>
#ifndef __BORLANDC__
#include <sys/utime.h>			/* for non-unicode version */
#endif
#undef near

/* Must be here to avoid conflicting with prototype in windows.h */
#define mkdir(a,b)	mkdir(a)

#define HAVE_FSYNC_WRITETHROUGH
#define HAVE_FSYNC_WRITETHROUGH_ONLY
#define ftruncate(a,b)	chsize(a,b)
/*
 *	Even though we don't support 'fsync' as a wal_sync_method,
 *	we do fsync() a few other places where _commit() is just fine.
 */
#define fsync(fd) _commit(fd)

#define USES_WINSOCK

/* defines for dynamic linking on Win32 platform */
#if defined(WIN32) || defined(__CYGWIN__)

#if __GNUC__ && ! defined (__declspec)
#error You need egcs 1.1 or newer for compiling!
#endif

#ifdef BUILDING_DLL
#define DLLIMPORT __declspec (dllexport)
#else							/* not BUILDING_DLL */
#define DLLIMPORT __declspec (dllimport)
#endif
#else							/* not CYGWIN, not MSVC, not MingW */

#define DLLIMPORT
#endif


/*
 *	IPC defines
 */
#undef HAVE_UNION_SEMUN
#define HAVE_UNION_SEMUN 1

#define IPC_RMID 256
#define IPC_CREAT 512
#define IPC_EXCL 1024
#define IPC_PRIVATE 234564
#define IPC_NOWAIT	2048
#define IPC_STAT 4096

#define EACCESS 2048
#define EIDRM 4096

#define SETALL 8192
#define GETNCNT 16384
#define GETVAL 65536
#define SETVAL 131072
#define GETPID 262144

/*
 *	Shared memory
 */
struct shmid_ds
{
	int			dummy;
	int			shm_nattch;
};

int			shmdt(const void *shmaddr);
void	   *shmat(int memId, void *shmaddr, int flag);
int			shmctl(int shmid, int flag, struct shmid_ds * dummy);
int			shmget(int memKey, int size, int flag);


/*
 *	Semaphores
 */
union semun
{
	int			val;
	struct semid_ds *buf;
	unsigned short *array;
};

struct sembuf
{
	int			sem_flg;
	int			sem_op;
	int			sem_num;
};

int			semctl(int semId, int semNum, int flag, union semun);
int			semget(int semKey, int semNum, int flags);
int			semop(int semId, struct sembuf * sops, int flag);


/*
 *	Signal stuff
 *	WIN32 doesn't have wait(), so the return value for children
 *	is simply the return value specified by the child, without
 *	any additional information on whether the child terminated
 *	on its own or via a signal.  These macros are also used
 *	to interpret the return value of system().
 */
#define WEXITSTATUS(w)	(w)
#define WIFEXITED(w)	(true)
#define WIFSIGNALED(w)	(false)
#define WTERMSIG(w)		(0)

#define sigmask(sig) ( 1 << ((sig)-1) )

/* Signal function return values */
#undef SIG_DFL
#undef SIG_ERR
#undef SIG_IGN
#define SIG_DFL ((pqsigfunc)0)
#define SIG_ERR ((pqsigfunc)-1)
#define SIG_IGN ((pqsigfunc)1)

/* Some extra signals */
#define SIGHUP				1
#define SIGQUIT				3
#define SIGTRAP				5
#define SIGABRT				22	/* Set to match W32 value -- not UNIX value */
#define SIGKILL				9
#define SIGPIPE				13
#define SIGALRM				14
#define SIGSTOP				17
#define SIGTSTP				18
#define SIGCONT				19
#define SIGCHLD				20
#define SIGTTIN				21
#define SIGTTOU				22	/* Same as SIGABRT -- no problem, I hope */
#define SIGWINCH			28
#ifndef __BORLANDC__
#define SIGUSR1				30
#define SIGUSR2				31
#endif

/*
 * New versions of mingw have gettimeofday() and also declare
 * struct timezone to support it.
 */
#ifndef HAVE_GETTIMEOFDAY
struct timezone
{
	int			tz_minuteswest; /* Minutes west of GMT.  */
	int			tz_dsttime;		/* Nonzero if DST is ever in effect.  */
};
#endif

/* for setitimer in backend/port/win32/timer.c */
#define ITIMER_REAL 0
struct itimerval
{
	struct timeval it_interval;
	struct timeval it_value;
};
int			setitimer(int which, const struct itimerval * value, struct itimerval * ovalue);


/*
 * Supplement to <sys/types.h>.
 *
 * Perl already has typedefs for uid_t and gid_t.
 */
#ifndef PLPERL_HAVE_UID_GID
typedef int uid_t;
typedef int gid_t;
#endif
typedef long key_t;

#ifdef WIN32_ONLY_COMPILER
typedef int pid_t;
#endif

/*
 * Supplement to <sys/stat.h>.
 */
#define lstat(path, sb) stat((path), (sb))

/*
 * Supplement to <fcntl.h>.
 * This is the same value as _O_NOINHERIT in the MS header file. This is
 * to ensure that we don't collide with a future definition. It means
 * we cannot use _O_NOINHERIT ourselves.
 */
#define O_DSYNC 0x0080

/*
 * Supplement to <errno.h>.
 */
#undef EAGAIN
#undef EINTR
#define EINTR WSAEINTR
#define EAGAIN WSAEWOULDBLOCK
#define EMSGSIZE WSAEMSGSIZE
#define EAFNOSUPPORT WSAEAFNOSUPPORT
#define EWOULDBLOCK WSAEWOULDBLOCK
#define ECONNRESET WSAECONNRESET
#define EINPROGRESS WSAEINPROGRESS
#define ENOBUFS WSAENOBUFS
#define EPROTONOSUPPORT WSAEPROTONOSUPPORT
#define ECONNREFUSED WSAECONNREFUSED
#define EBADFD WSAENOTSOCK
#define EOPNOTSUPP WSAEOPNOTSUPP


/* In backend/port/win32/signal.c */
extern DLLIMPORT volatile int pg_signal_queue;
extern DLLIMPORT int pg_signal_mask;
extern HANDLE pgwin32_signal_event;
extern HANDLE pgwin32_initial_signal_pipe;

#define UNBLOCKED_SIGNAL_QUEUE()	(pg_signal_queue & ~pg_signal_mask)


void		pgwin32_signal_initialize(void);
HANDLE		pgwin32_create_signal_listener(pid_t pid);
void		pgwin32_dispatch_queued_signals(void);
void		pg_queue_signal(int signum);

/* In backend/port/win32/socket.c */
#ifndef FRONTEND
#define socket(af, type, protocol) pgwin32_socket(af, type, protocol)
#define accept(s, addr, addrlen) pgwin32_accept(s, addr, addrlen)
#define connect(s, name, namelen) pgwin32_connect(s, name, namelen)
#define select(n, r, w, e, timeout) pgwin32_select(n, r, w, e, timeout)
#define recv(s, buf, len, flags) pgwin32_recv(s, buf, len, flags)
#define send(s, buf, len, flags) pgwin32_send(s, buf, len, flags)

SOCKET		pgwin32_socket(int af, int type, int protocol);
SOCKET		pgwin32_accept(SOCKET s, struct sockaddr * addr, int *addrlen);
int			pgwin32_connect(SOCKET s, const struct sockaddr * name, int namelen);
int			pgwin32_select(int nfds, fd_set *readfs, fd_set *writefds, fd_set *exceptfds, const struct timeval * timeout);
int			pgwin32_recv(SOCKET s, char *buf, int len, int flags);
int			pgwin32_send(SOCKET s, char *buf, int len, int flags);

const char *pgwin32_socket_strerror(int err);
int			pgwin32_waitforsinglesocket(SOCKET s, int what, int timeout);

/* in backend/port/win32/security.c */
extern int	pgwin32_is_admin(void);
extern int	pgwin32_is_service(void);
#endif

/* in port/win32error.c */
extern void _dosmaperr(unsigned long);


/* Things that exist in MingW headers, but need to be added to MSVC */
#ifdef WIN32_ONLY_COMPILER
#ifndef __BORLANDC__
typedef long ssize_t;
typedef unsigned short mode_t;
#endif

/*
 *	Certain "standard edition" versions of MSVC throw a warning
 *	that later generates an error for "inline" statements, but
 *	__inline seems to work.  e.g.  Microsoft Visual C++ .NET
 *	Version 7.1.3088
 */
#define inline __inline
#define __inline__ __inline

#ifndef __BORLANDC__
#define _S_IRWXU	(_S_IREAD | _S_IWRITE | _S_IEXEC)
#define _S_IXUSR	_S_IEXEC
#define _S_IWUSR	_S_IWRITE
#define _S_IRUSR	_S_IREAD
#define S_IRUSR		_S_IRUSR
#define S_IWUSR		_S_IWUSR
#define S_IXUSR		_S_IXUSR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#define F_OK 0
#define W_OK 2
#define R_OK 4

#define isinf(x) ((_fpclass(x) == _FPCLASS_PINF) || (_fpclass(x) == _FPCLASS_NINF))
#define isnan(x) _isnan(x)
#define finite(x) _finite(x)

#ifndef			BIG_ENDIAN
#define			BIG_ENDIAN		4321
#endif
#ifndef			LITTLE_ENDIAN
#define			LITTLE_ENDIAN	1234
#endif
#ifndef			PDP_ENDIAN
#define			PDP_ENDIAN		3412
#endif

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* Pulled from Makefile.port in mingw */
#define DLSUFFIX ".dll"

#endif
