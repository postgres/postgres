/* $Header: /cvsroot/pgsql/src/include/port/win32.h,v 1.7 2003/04/18 01:03:42 momjian Exp $ */

#include <port/win32defs.h>

#define USES_WINSOCK
#define NOFILE		  100

/* defines for dynamic linking on Win32 platform */
#ifdef __CYGWIN__

#if __GNUC__ && ! defined (__declspec)
#error You need egcs 1.1 or newer for compiling!
#endif

#ifdef BUILDING_DLL
#define DLLIMPORT __declspec (dllexport)
#else							/* not BUILDING_DLL */
#define DLLIMPORT __declspec (dllimport)
#endif

#elif defined(WIN32) && defined(_MSC_VER)		/* not CYGWIN */

#if defined(_DLL)
#define DLLIMPORT __declspec (dllexport)
#else							/* not _DLL */
#define DLLIMPORT __declspec (dllimport)
#endif

#else							/* not CYGWIN, not MSVC */

#define DLLIMPORT

#endif

/*
 * Supplement to <sys/types.h>.
 */
#define uid_t int
#define gid_t int
#define pid_t unsigned long
#define ssize_t int
#define mode_t int
#define key_t long
#define ushort unsigned short

/*
 * Supplement to <sys/stat.h>.
 */
#define lstat slat

#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)

#define S_IRUSR _S_IREAD
#define S_IWUSR _S_IWRITE
#define S_IXUSR _S_IEXEC
#define S_IRWXU (_S_IREAD | _S_IWRITE | _S_IEXEC)

/*
 * Supplement to <errno.h>.
 */
#include <errno.h>
#undef EAGAIN
#undef EINTR
#define EINTR WSAEINTR
#define EAGAIN WSAEWOULDBLOCK
#define EMSGSIZE WSAEMSGSIZE
#define EAFNOSUPPORT WSAEAFNOSUPPORT
#define EWOULDBLOCK WSAEWOULDBLOCK
#define ECONNRESET WSAECONNRESET
#define EINPROGRESS WSAEINPROGRESS

/*
 * Supplement to <math.h>.
 */
#define isnan _isnan
#define finite _finite
extern double rint(double x);

/*
 * Supplement to <stdio.h>.
 */
#define snprintf _snprintf
#define vsnprintf _vsnprintf


