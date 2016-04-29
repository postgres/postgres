/* src/include/port/win32.h */

#if defined(_MSC_VER) || defined(__BORLANDC__)
#define WIN32_ONLY_COMPILER
#endif

/*
 * Make sure _WIN32_WINNT has the minimum required value.
 * Leave a higher value in place. When building with at least Visual
 * Studio 2015 the minimum requirement is Windows Vista (0x0600) to
 * get support for GetLocaleInfoEx() with locales. For everything else
 * the minumum version is Windows XP (0x0501).
 * Also for VS2015, add a define that stops compiler complaints about
 * using the old Winsock API.
 */
#if defined(_MSC_VER) && _MSC_VER >= 1900
#define  _WINSOCK_DEPRECATED_NO_WARNINGS
#define MIN_WINNT 0x0600
#else
#define MIN_WINNT 0x0501
#endif

#if defined(_WIN32_WINNT) && _WIN32_WINNT < MIN_WINNT
#undef _WIN32_WINNT
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT MIN_WINNT
#endif

/*
 * Always build with SSPI support. Keep it as a #define in case
 * we want a switch to disable it sometime in the future.
 */
#ifndef __BORLANDC__
#define ENABLE_SSPI 1
#endif

/* undefine and redefine after #include */
#undef mkdir

#undef ERROR

/*
 * The Mingw64 headers choke if this is already defined - they
 * define it themselves.
 */
#if !defined(__MINGW64_VERSION_MAJOR) || defined(WIN32_ONLY_COMPILER)
#define _WINSOCKAPI_
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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

/* defines for dynamic linking on Win32 platform
 *
 *	http://support.microsoft.com/kb/132044
 *	http://msdn.microsoft.com/en-us/library/8fskxacy(v=vs.80).aspx
 *	http://msdn.microsoft.com/en-us/library/a90k134d(v=vs.80).aspx
 */

#if defined(WIN32) || defined(__CYGWIN__)

#ifdef BUILDING_DLL
#define PGDLLIMPORT __declspec (dllexport)
#else							/* not BUILDING_DLL */
#define PGDLLIMPORT __declspec (dllimport)
#endif

#ifdef _MSC_VER
#define PGDLLEXPORT __declspec (dllexport)
#else
#define PGDLLEXPORT
#endif
#else							/* not CYGWIN, not MSVC, not MingW */
#define PGDLLIMPORT
#define PGDLLEXPORT
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
 * WIN32 does not provide 64-bit off_t, but does provide the functions operating
 * with 64-bit offsets.
 */
#define pgoff_t __int64
#ifdef WIN32_ONLY_COMPILER
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
 *
 * We redefine network-related Berkeley error symbols as the corresponding WSA
 * constants.  This allows elog.c to recognize them as being in the Winsock
 * error code range and pass them off to pgwin32_socket_strerror(), since
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


/* In backend/port/win32/signal.c */
extern PGDLLIMPORT volatile int pg_signal_queue;
extern PGDLLIMPORT int pg_signal_mask;
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
#define bind(s, addr, addrlen) pgwin32_bind(s, addr, addrlen)
#define listen(s, backlog) pgwin32_listen(s, backlog)
#define accept(s, addr, addrlen) pgwin32_accept(s, addr, addrlen)
#define connect(s, name, namelen) pgwin32_connect(s, name, namelen)
#define select(n, r, w, e, timeout) pgwin32_select(n, r, w, e, timeout)
#define recv(s, buf, len, flags) pgwin32_recv(s, buf, len, flags)
#define send(s, buf, len, flags) pgwin32_send(s, buf, len, flags)

SOCKET		pgwin32_socket(int af, int type, int protocol);
int			pgwin32_bind(SOCKET s, struct sockaddr * addr, int addrlen);
int			pgwin32_listen(SOCKET s, int backlog);
SOCKET		pgwin32_accept(SOCKET s, struct sockaddr * addr, int *addrlen);
int			pgwin32_connect(SOCKET s, const struct sockaddr * name, int namelen);
int			pgwin32_select(int nfds, fd_set *readfs, fd_set *writefds, fd_set *exceptfds, const struct timeval * timeout);
int			pgwin32_recv(SOCKET s, char *buf, int len, int flags);
int			pgwin32_send(SOCKET s, const void *buf, int len, int flags);

const char *pgwin32_socket_strerror(int err);
int			pgwin32_waitforsinglesocket(SOCKET s, int what, int timeout);

extern int	pgwin32_noblock;

/* in backend/port/win32/security.c */
extern int	pgwin32_is_admin(void);
extern int	pgwin32_is_service(void);
#endif

/* in backend/port/win32_shmem.c */
extern int	pgwin32_ReserveSharedMemoryRegion(HANDLE);

/* in backend/port/win32/crashdump.c */
extern void pgwin32_install_crashdump_handler(void);

/* in port/win32error.c */
extern void _dosmaperr(unsigned long);

/* in port/win32env.c */
extern int	pgwin32_putenv(const char *);
extern void pgwin32_unsetenv(const char *);

#define putenv(x) pgwin32_putenv(x)
#define unsetenv(x) pgwin32_unsetenv(x)

/* Things that exist in MingW headers, but need to be added to MSVC & BCC */
#ifdef WIN32_ONLY_COMPILER

#ifndef _WIN64
typedef long ssize_t;
#else
typedef __int64 ssize_t;
#endif

#ifndef __BORLANDC__
typedef unsigned short mode_t;

#define S_IRUSR _S_IREAD
#define S_IWUSR _S_IWRITE
#define S_IXUSR _S_IEXEC
#define S_IRWXU (S_IRUSR | S_IWUSR | S_IXUSR)
/* see also S_IRGRP etc below */
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif   /* __BORLANDC__ */

#define F_OK 0
#define W_OK 2
#define R_OK 4

#if (_MSC_VER < 1800)
#define isinf(x) ((_fpclass(x) == _FPCLASS_PINF) || (_fpclass(x) == _FPCLASS_NINF))
#define isnan(x) _isnan(x)
#endif

/* Pulled from Makefile.port in mingw */
#define DLSUFFIX ".dll"

#ifdef __BORLANDC__

/* for port/dirent.c */
#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD) -1)
#endif

/* for port/open.c */
#ifndef O_RANDOM
#define O_RANDOM		0x0010	/* File access is primarily random */
#define O_SEQUENTIAL	0x0020	/* File access is primarily sequential */
#define O_TEMPORARY		0x0040	/* Temporary file bit */
#define O_SHORT_LIVED	0x1000	/* Temporary storage file, try not to flush */
#define _O_SHORT_LIVED	O_SHORT_LIVED
#endif   /* ifndef O_RANDOM */
#endif   /* __BORLANDC__ */
#endif   /* WIN32_ONLY_COMPILER */

/* These aren't provided by either MingW or MSVC */
#ifndef __BORLANDC__
#define S_IRGRP 0
#define S_IWGRP 0
#define S_IXGRP 0
#define S_IRWXG 0
#define S_IROTH 0
#define S_IWOTH 0
#define S_IXOTH 0
#define S_IRWXO 0

#endif   /* __BORLANDC__ */
