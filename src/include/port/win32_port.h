/*-------------------------------------------------------------------------
 *
 * win32_port.h
 *	  Windows-specific compatibility stuff.
 *
 * Note this is read in MinGW as well as native Windows builds,
 * but not in Cygwin builds.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port/win32_port.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_WIN32_PORT_H
#define PG_WIN32_PORT_H

/*
 * Always build with SSPI support. Keep it as a #define in case
 * we want a switch to disable it sometime in the future.
 */
#define ENABLE_SSPI 1

/* undefine and redefine after #include */
#undef mkdir

#undef ERROR

/*
 * VS2013 and later issue warnings about using the old Winsock API,
 * which we don't really want to hear about.
 */
#ifdef _MSC_VER
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

/*
 * The MinGW64 headers choke if this is already defined - they
 * define it themselves.
 */
#if !defined(__MINGW64_VERSION_MAJOR) || defined(_MSC_VER)
#define _WINSOCKAPI_
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#undef small
#include <process.h>
#include <signal.h>
#include <direct.h>
#include <sys/utime.h>			/* for non-unicode version */
#undef near
#include <sys/stat.h>			/* needed before sys/stat hacking below */

/* Must be here to avoid conflicting with prototype in windows.h */
#define mkdir(a,b)	mkdir(a)

#define ftruncate(a,b)	chsize(a,b)

/* Windows doesn't have fsync() as such, use _commit() */
#define fsync(fd) _commit(fd)

/*
 * For historical reasons, we allow setting wal_sync_method to
 * fsync_writethrough on Windows, even though it's really identical to fsync
 * (both code paths wind up at _commit()).
 */
#define HAVE_FSYNC_WRITETHROUGH
#define FSYNC_WRITETHROUGH_IS_FSYNC

#define USES_WINSOCK

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
#ifndef EIDRM
#define EIDRM 4096
#endif

#define SETALL 8192
#define GETNCNT 16384
#define GETVAL 65536
#define SETVAL 131072
#define GETPID 262144


/*
 *	Signal stuff
 *
 *	For WIN32, there is no wait() call so there are no wait() macros
 *	to interpret the return value of system().  Instead, system()
 *	return values < 0x100 are used for exit() termination, and higher
 *	values are used to indicated non-exit() termination, which is
 *	similar to a unix-style signal exit (think SIGSEGV ==
 *	STATUS_ACCESS_VIOLATION).  Return values are broken up into groups:
 *
 *	http://msdn2.microsoft.com/en-gb/library/aa489609.aspx
 *
 *		NT_SUCCESS			0 - 0x3FFFFFFF
 *		NT_INFORMATION		0x40000000 - 0x7FFFFFFF
 *		NT_WARNING			0x80000000 - 0xBFFFFFFF
 *		NT_ERROR			0xC0000000 - 0xFFFFFFFF
 *
 *	Effectively, we don't care on the severity of the return value from
 *	system(), we just need to know if it was because of exit() or generated
 *	by the system, and it seems values >= 0x100 are system-generated.
 *	See this URL for a list of WIN32 STATUS_* values:
 *
 *		Wine (URL used in our error messages) -
 *			http://source.winehq.org/source/include/ntstatus.h
 *		Descriptions - http://www.comp.nus.edu.sg/~wuyongzh/my_doc/ntstatus.txt
 *		MS SDK - http://www.nologs.com/ntstatus.html
 *
 *	It seems the exception lists are in both ntstatus.h and winnt.h, but
 *	ntstatus.h has a more comprehensive list, and it only contains
 *	exception values, rather than winnt, which contains lots of other
 *	things:
 *
 *		http://www.microsoft.com/msj/0197/exception/exception.aspx
 *
 *		The ExceptionCode parameter is the number that the operating system
 *		assigned to the exception. You can see a list of various exception codes
 *		in WINNT.H by searching for #defines that start with "STATUS_". For
 *		example, the code for the all-too-familiar STATUS_ACCESS_VIOLATION is
 *		0xC0000005. A more complete set of exception codes can be found in
 *		NTSTATUS.H from the Windows NT DDK.
 *
 *	Some day we might want to print descriptions for the most common
 *	exceptions, rather than printing an include file name.  We could use
 *	RtlNtStatusToDosError() and pass to FormatMessage(), which can print
 *	the text of error values, but MinGW does not support
 *	RtlNtStatusToDosError().
 */
#define WIFEXITED(w)	(((w) & 0XFFFFFF00) == 0)
#define WIFSIGNALED(w)	(!WIFEXITED(w))
#define WEXITSTATUS(w)	(w)
#define WTERMSIG(w)		(w)

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
#define SIGWINCH			28
#define SIGUSR1				30
#define SIGUSR2				31

/*
 * New versions of MinGW have gettimeofday() and also declare
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

int			setitimer(int which, const struct itimerval *value, struct itimerval *ovalue);

/*
 * WIN32 does not provide 64-bit off_t, but does provide the functions operating
 * with 64-bit offsets.
 */
#define pgoff_t __int64
#ifdef _MSC_VER
#define fseeko(stream, offset, origin) _fseeki64(stream, offset, origin)
#define ftello(stream) _ftelli64(stream)
#else
#ifndef fseeko
#define fseeko(stream, offset, origin) fseeko64(stream, offset, origin)
#endif
#ifndef ftello
#define ftello(stream) ftello64(stream)
#endif
#endif

/*
 *	Win32 also doesn't have symlinks, but we can emulate them with
 *	junction points on newer Win32 versions.
 *
 *	Cygwin has its own symlinks which work on Win95/98/ME where
 *	junction points don't, so use those instead.  We have no way of
 *	knowing what type of system Cygwin binaries will be run on.
 *		Note: Some CYGWIN includes might #define WIN32.
 */
extern int	pgsymlink(const char *oldpath, const char *newpath);
extern int	pgreadlink(const char *path, char *buf, size_t size);
extern bool pgwin32_is_junction(const char *path);

#define symlink(oldpath, newpath)	pgsymlink(oldpath, newpath)
#define readlink(path, buf, size)	pgreadlink(path, buf, size)

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

#ifdef _MSC_VER
typedef int pid_t;
#endif

/*
 * Supplement to <sys/stat.h>.
 *
 * We must pull in sys/stat.h before this part, else our overrides lose.
 */
#define lstat(path, sb) stat(path, sb)

/*
 * stat() is not guaranteed to set the st_size field on win32, so we
 * redefine it to our own implementation that is.
 *
 * Some frontends don't need the size from stat, so if UNSAFE_STAT_OK
 * is defined we don't bother with this.
 */
#ifndef UNSAFE_STAT_OK
extern int	pgwin32_safestat(const char *path, struct stat *buf);
#define stat(a,b) pgwin32_safestat(a,b)
#endif

/* These macros are not provided by older MinGW, nor by MSVC */
#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#endif
#ifndef S_IWUSR
#define S_IWUSR _S_IWRITE
#endif
#ifndef S_IXUSR
#define S_IXUSR _S_IEXEC
#endif
#ifndef S_IRWXU
#define S_IRWXU (S_IRUSR | S_IWUSR | S_IXUSR)
#endif
#ifndef S_IRGRP
#define S_IRGRP 0
#endif
#ifndef S_IWGRP
#define S_IWGRP 0
#endif
#ifndef S_IXGRP
#define S_IXGRP 0
#endif
#ifndef S_IRWXG
#define S_IRWXG 0
#endif
#ifndef S_IROTH
#define S_IROTH 0
#endif
#ifndef S_IWOTH
#define S_IWOTH 0
#endif
#ifndef S_IXOTH
#define S_IXOTH 0
#endif
#ifndef S_IRWXO
#define S_IRWXO 0
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

/*
 * Supplement to <fcntl.h>.
 * This is the same value as _O_NOINHERIT in the MS header file. This is
 * to ensure that we don't collide with a future definition. It means
 * we cannot use _O_NOINHERIT ourselves.
 */
#define O_DSYNC 0x0080

/*
 * Supplement to <errno.h>.
 *
 * We redefine network-related Berkeley error symbols as the corresponding WSA
 * constants. This allows strerror.c to recognize them as being in the Winsock
 * error code range and pass them off to win32_socket_strerror(), since
 * Windows' version of plain strerror() won't cope.  Note that this will break
 * if these names are used for anything else besides Windows Sockets errors.
 * See TranslateSocketError() when changing this list.
 */
#undef EAGAIN
#define EAGAIN WSAEWOULDBLOCK
#undef EINTR
#define EINTR WSAEINTR
#undef EMSGSIZE
#define EMSGSIZE WSAEMSGSIZE
#undef EAFNOSUPPORT
#define EAFNOSUPPORT WSAEAFNOSUPPORT
#undef EWOULDBLOCK
#define EWOULDBLOCK WSAEWOULDBLOCK
#undef ECONNABORTED
#define ECONNABORTED WSAECONNABORTED
#undef ECONNRESET
#define ECONNRESET WSAECONNRESET
#undef EINPROGRESS
#define EINPROGRESS WSAEINPROGRESS
#undef EISCONN
#define EISCONN WSAEISCONN
#undef ENOBUFS
#define ENOBUFS WSAENOBUFS
#undef EPROTONOSUPPORT
#define EPROTONOSUPPORT WSAEPROTONOSUPPORT
#undef ECONNREFUSED
#define ECONNREFUSED WSAECONNREFUSED
#undef ENOTSOCK
#define ENOTSOCK WSAENOTSOCK
#undef EOPNOTSUPP
#define EOPNOTSUPP WSAEOPNOTSUPP
#undef EADDRINUSE
#define EADDRINUSE WSAEADDRINUSE
#undef EADDRNOTAVAIL
#define EADDRNOTAVAIL WSAEADDRNOTAVAIL
#undef EHOSTUNREACH
#define EHOSTUNREACH WSAEHOSTUNREACH
#undef ENOTCONN
#define ENOTCONN WSAENOTCONN

/*
 * Locale stuff.
 *
 * Extended locale functions with gratuitous underscore prefixes.
 * (These APIs are nevertheless fully documented by Microsoft.)
 */
#define locale_t _locale_t
#define tolower_l _tolower_l
#define toupper_l _toupper_l
#define towlower_l _towlower_l
#define towupper_l _towupper_l
#define isdigit_l _isdigit_l
#define iswdigit_l _iswdigit_l
#define isalpha_l _isalpha_l
#define iswalpha_l _iswalpha_l
#define isalnum_l _isalnum_l
#define iswalnum_l _iswalnum_l
#define isupper_l _isupper_l
#define iswupper_l _iswupper_l
#define islower_l _islower_l
#define iswlower_l _iswlower_l
#define isgraph_l _isgraph_l
#define iswgraph_l _iswgraph_l
#define isprint_l _isprint_l
#define iswprint_l _iswprint_l
#define ispunct_l _ispunct_l
#define iswpunct_l _iswpunct_l
#define isspace_l _isspace_l
#define iswspace_l _iswspace_l
#define strcoll_l _strcoll_l
#define strxfrm_l _strxfrm_l
#define wcscoll_l _wcscoll_l
#define wcstombs_l _wcstombs_l
#define mbstowcs_l _mbstowcs_l

/*
 * Versions of libintl >= 0.18? try to replace setlocale() with a macro
 * to their own versions.  Remove the macro, if it exists, because it
 * ends up calling the wrong version when the backend and libintl use
 * different versions of msvcrt.
 */
#if defined(setlocale)
#undef setlocale
#endif

/*
 * Define our own wrapper macro around setlocale() to work around bugs in
 * Windows' native setlocale() function.
 */
extern char *pgwin32_setlocale(int category, const char *locale);

#define setlocale(a,b) pgwin32_setlocale(a,b)


/* In backend/port/win32/signal.c */
extern PGDLLIMPORT volatile int pg_signal_queue;
extern PGDLLIMPORT int pg_signal_mask;
extern HANDLE pgwin32_signal_event;
extern HANDLE pgwin32_initial_signal_pipe;

#define UNBLOCKED_SIGNAL_QUEUE()	(pg_signal_queue & ~pg_signal_mask)
#define PG_SIGNAL_COUNT 32

void		pgwin32_signal_initialize(void);
HANDLE		pgwin32_create_signal_listener(pid_t pid);
void		pgwin32_dispatch_queued_signals(void);
void		pg_queue_signal(int signum);

/* In src/port/kill.c */
#define kill(pid,sig)	pgkill(pid,sig)
extern int	pgkill(int pid, int sig);

/* In backend/port/win32/socket.c */
#ifndef FRONTEND
#define socket(af, type, protocol) pgwin32_socket(af, type, protocol)
#define bind(s, addr, addrlen) pgwin32_bind(s, addr, addrlen)
#define listen(s, backlog) pgwin32_listen(s, backlog)
#define accept(s, addr, addrlen) pgwin32_accept(s, addr, addrlen)
#define connect(s, name, namelen) pgwin32_connect(s, name, namelen)
#define select(n, r, w, e, timeout) pgwin32_select(n, r, w, e, timeout)
#define recv(s, buf, len, flags) pgwin32_recv(s, buf, len, flags)
#define send(s, buf, len, flags) pgwin32_send(s, buf, len, flags)

SOCKET		pgwin32_socket(int af, int type, int protocol);
int			pgwin32_bind(SOCKET s, struct sockaddr *addr, int addrlen);
int			pgwin32_listen(SOCKET s, int backlog);
SOCKET		pgwin32_accept(SOCKET s, struct sockaddr *addr, int *addrlen);
int			pgwin32_connect(SOCKET s, const struct sockaddr *name, int namelen);
int			pgwin32_select(int nfds, fd_set *readfs, fd_set *writefds, fd_set *exceptfds, const struct timeval *timeout);
int			pgwin32_recv(SOCKET s, char *buf, int len, int flags);
int			pgwin32_send(SOCKET s, const void *buf, int len, int flags);
int			pgwin32_waitforsinglesocket(SOCKET s, int what, int timeout);

extern int	pgwin32_noblock;

#endif							/* FRONTEND */

/* in backend/port/win32_shmem.c */
extern int	pgwin32_ReserveSharedMemoryRegion(HANDLE);

/* in backend/port/win32/crashdump.c */
extern void pgwin32_install_crashdump_handler(void);

/* in port/win32error.c */
extern void _dosmaperr(unsigned long);

/* in port/win32env.c */
extern int	pgwin32_putenv(const char *);
extern void pgwin32_unsetenv(const char *);

/* in port/win32security.c */
extern int	pgwin32_is_service(void);
extern int	pgwin32_is_admin(void);

/* Windows security token manipulation (in src/common/exec.c) */
extern BOOL AddUserToTokenDacl(HANDLE hToken);

#define putenv(x) pgwin32_putenv(x)
#define unsetenv(x) pgwin32_unsetenv(x)

/* Things that exist in MinGW headers, but need to be added to MSVC */
#ifdef _MSC_VER

#ifndef _WIN64
typedef long ssize_t;
#else
typedef __int64 ssize_t;
#endif

typedef unsigned short mode_t;

#define F_OK 0
#define W_OK 2
#define R_OK 4

/*
 * isinf() and isnan() should per spec be in <math.h>, but MSVC older than
 * 2013 does not have them there.  It does have _fpclass() and _isnan(), but
 * they're in <float.h>, so include that here even though it means float.h
 * percolates to our whole tree.  Recent versions don't require any of this.
 */
#if (_MSC_VER < 1800)
#include <float.h>
#define isinf(x) ((_fpclass(x) == _FPCLASS_PINF) || (_fpclass(x) == _FPCLASS_NINF))
#define isnan(x) _isnan(x)
#endif

/* Pulled from Makefile.port in MinGW */
#define DLSUFFIX ".dll"

#endif							/* _MSC_VER */

#if (defined(_MSC_VER) && (_MSC_VER < 1900)) || \
	defined(__MINGW32__) || defined(__MINGW64__)
/*
 * VS2013 has a strtof() that seems to give correct answers for valid input,
 * even on the rounding edge cases, but which doesn't handle out-of-range
 * input correctly. Work around that.
 *
 * Mingw claims to have a strtof, and my reading of its source code suggests
 * that it ought to work (and not need this hack), but the regression test
 * results disagree with me; whether this is a version issue or not is not
 * clear. However, using our wrapper (and the misrounded-input variant file,
 * already required for supporting ancient systems) can't make things any
 * worse, except for a tiny performance loss when reading zeros.
 *
 * See also cygwin.h for another instance of this.
 */
#define HAVE_BUGGY_STRTOF 1
#endif

#endif							/* PG_WIN32_PORT_H */
