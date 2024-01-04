/*-------------------------------------------------------------------------
 *
 * win32_port.h
 *	  Windows-specific compatibility stuff.
 *
 * Note this is read in MinGW as well as native Windows builds,
 * but not in Cygwin builds.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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

/*
 * windows.h includes a lot of other headers, slowing down compilation
 * significantly.  WIN32_LEAN_AND_MEAN reduces that a bit. It'd be better to
 * remove the include of windows.h (as well as indirect inclusions of it) from
 * such a central place, but until then...
 *
 * To be able to include ntstatus.h tell windows.h to not declare NTSTATUS by
 * temporarily defining UMDF_USING_NTSTATUS, otherwise we'll get warning about
 * macro redefinitions, as windows.h also defines NTSTATUS (yuck). That in
 * turn requires including ntstatus.h, winternl.h to get common symbols.
 */
#define WIN32_LEAN_AND_MEAN
#define UMDF_USING_NTSTATUS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ntstatus.h>
#include <winternl.h>

#undef small
#include <process.h>
#include <signal.h>
#include <direct.h>
#undef near

/* needed before sys/stat hacking below: */
#define fstat microsoft_native_fstat
#define stat microsoft_native_stat
#include <sys/stat.h>
#undef fstat
#undef stat

/* Must be here to avoid conflicting with prototype in windows.h */
#define mkdir(a,b)	mkdir(a)

#define ftruncate(a,b)	chsize(a,b)

/* Windows doesn't have fsync() as such, use _commit() */
#define fsync(fd) _commit(fd)

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
 *	values are used to indicate non-exit() termination, which is
 *	similar to a unix-style signal exit (think SIGSEGV ==
 *	STATUS_ACCESS_VIOLATION).  Return values are broken up into groups:
 *
 *	https://docs.microsoft.com/en-us/windows-hardware/drivers/kernel/using-ntstatus-values
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
 *		Descriptions -
 *			https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/596a1078-e883-4972-9bbc-49e60bebca55
 *
 *	The comprehensive exception list is included in ntstatus.h from the
 *	Windows Driver Kit (WDK).  A subset of the list is also included in
 *	winnt.h from the Windows SDK.  Defining WIN32_NO_STATUS before including
 *	windows.h helps to avoid any conflicts.
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

/* MinGW has gettimeofday(), but MSVC doesn't */
#ifdef _MSC_VER
/* Last parameter not used */
extern int	gettimeofday(struct timeval *tp, void *tzp);
#endif

/* for setitimer in backend/port/win32/timer.c */
#define ITIMER_REAL 0
struct itimerval
{
	struct timeval it_interval;
	struct timeval it_value;
};

int			setitimer(int which, const struct itimerval *value, struct itimerval *ovalue);

/* Convenience wrapper for GetFileType() */
extern DWORD pgwin32_get_file_type(HANDLE hFile);

/*
 * WIN32 does not provide 64-bit off_t, but does provide the functions operating
 * with 64-bit offsets.  Also, fseek() might not give an error for unseekable
 * streams, so harden that function with our version.
 */
#define pgoff_t __int64

#ifdef _MSC_VER
extern int	_pgfseeko64(FILE *stream, pgoff_t offset, int origin);
extern pgoff_t _pgftello64(FILE *stream);
#define fseeko(stream, offset, origin) _pgfseeko64(stream, offset, origin)
#define ftello(stream) _pgftello64(stream)
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
 *
 * stat() is not guaranteed to set the st_size field on win32, so we
 * redefine it to our own implementation.  See src/port/win32stat.c.
 *
 * The struct stat is 32 bit in MSVC, so we redefine it as a copy of
 * struct __stat64.  This also fixes the struct size for MINGW builds.
 */
struct stat						/* This should match struct __stat64 */
{
	_dev_t		st_dev;
	_ino_t		st_ino;
	unsigned short st_mode;
	short		st_nlink;
	short		st_uid;
	short		st_gid;
	_dev_t		st_rdev;
	__int64		st_size;
	__time64_t	st_atime;
	__time64_t	st_mtime;
	__time64_t	st_ctime;
};

extern int	_pgfstat64(int fileno, struct stat *buf);
extern int	_pgstat64(const char *name, struct stat *buf);
extern int	_pglstat64(const char *name, struct stat *buf);

#define fstat(fileno, sb)	_pgfstat64(fileno, sb)
#define stat(path, sb)		_pgstat64(path, sb)
#define lstat(path, sb)		_pglstat64(path, sb)

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
 * In order for lstat() to be able to report junction points as symlinks, we
 * need to hijack a bit in st_mode, since neither MSVC nor MinGW provides
 * S_ISLNK and there aren't any spare bits.  We'll steal the one for character
 * devices, because we don't otherwise make use of those.
 */
#ifdef S_ISLNK
#error "S_ISLNK is already defined"
#endif
#ifdef S_IFLNK
#error "S_IFLNK is already defined"
#endif
#define S_IFLNK S_IFCHR
#define S_ISLNK(m) (((m) & S_IFLNK) == S_IFLNK)

/*
 * Supplement to <fcntl.h>.
 * This is the same value as _O_NOINHERIT in the MS header file. This is
 * to ensure that we don't collide with a future definition. It means
 * we cannot use _O_NOINHERIT ourselves.
 */
#define O_DSYNC 0x0080

/*
 * Our open() replacement does not create inheritable handles, so it is safe to
 * ignore O_CLOEXEC.  (If we were using Windows' own open(), it might be
 * necessary to convert this to _O_NOINHERIT.)
 */
#define O_CLOEXEC 0

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
#undef EHOSTDOWN
#define EHOSTDOWN WSAEHOSTDOWN
#undef EHOSTUNREACH
#define EHOSTUNREACH WSAEHOSTUNREACH
#undef ENETDOWN
#define ENETDOWN WSAENETDOWN
#undef ENETRESET
#define ENETRESET WSAENETRESET
#undef ENETUNREACH
#define ENETUNREACH WSAENETUNREACH
#undef ENOTCONN
#define ENOTCONN WSAENOTCONN
#undef ETIMEDOUT
#define ETIMEDOUT WSAETIMEDOUT

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
extern PGDLLIMPORT HANDLE pgwin32_signal_event;
extern PGDLLIMPORT HANDLE pgwin32_initial_signal_pipe;

#define UNBLOCKED_SIGNAL_QUEUE()	(pg_signal_queue & ~pg_signal_mask)
#define PG_SIGNAL_COUNT 32

extern void pgwin32_signal_initialize(void);
extern HANDLE pgwin32_create_signal_listener(pid_t pid);
extern void pgwin32_dispatch_queued_signals(void);
extern void pg_queue_signal(int signum);

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

extern SOCKET pgwin32_socket(int af, int type, int protocol);
extern int	pgwin32_bind(SOCKET s, struct sockaddr *addr, int addrlen);
extern int	pgwin32_listen(SOCKET s, int backlog);
extern SOCKET pgwin32_accept(SOCKET s, struct sockaddr *addr, int *addrlen);
extern int	pgwin32_connect(SOCKET s, const struct sockaddr *name, int namelen);
extern int	pgwin32_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timeval *timeout);
extern int	pgwin32_recv(SOCKET s, char *buf, int len, int flags);
extern int	pgwin32_send(SOCKET s, const void *buf, int len, int flags);
extern int	pgwin32_waitforsinglesocket(SOCKET s, int what, int timeout);

extern PGDLLIMPORT int pgwin32_noblock;

#endif							/* FRONTEND */

/* in backend/port/win32_shmem.c */
extern int	pgwin32_ReserveSharedMemoryRegion(HANDLE);

/* in backend/port/win32/crashdump.c */
extern void pgwin32_install_crashdump_handler(void);

/* in port/win32dlopen.c */
extern void *dlopen(const char *file, int mode);
extern void *dlsym(void *handle, const char *symbol);
extern int	dlclose(void *handle);
extern char *dlerror(void);

#define RTLD_NOW 1
#define RTLD_GLOBAL 0

/* in port/win32error.c */
extern void _dosmaperr(unsigned long);

/* in port/win32env.c */
extern int	pgwin32_putenv(const char *);
extern int	pgwin32_setenv(const char *name, const char *value, int overwrite);
extern int	pgwin32_unsetenv(const char *name);

#define putenv(x) pgwin32_putenv(x)
#define setenv(x,y,z) pgwin32_setenv(x,y,z)
#define unsetenv(x) pgwin32_unsetenv(x)

/* in port/win32security.c */
extern int	pgwin32_is_service(void);
extern int	pgwin32_is_admin(void);

/* Windows security token manipulation (in src/common/exec.c) */
extern BOOL AddUserToTokenDacl(HANDLE hToken);

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

#endif							/* _MSC_VER */

#if defined(__MINGW32__) || defined(__MINGW64__)
/*
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

/* in port/win32pread.c */
extern ssize_t pg_pread(int fd, void *buf, size_t nbyte, off_t offset);

/* in port/win32pwrite.c */
extern ssize_t pg_pwrite(int fd, const void *buf, size_t nbyte, off_t offset);

#endif							/* PG_WIN32_PORT_H */
