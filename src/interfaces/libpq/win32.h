#ifndef __win32_h_included
#define __win32_h_included

/*
 * strcasecmp() is not in Windows, stricmp is, though
 */
#define strcasecmp(a,b) stricmp(a,b)
#define strncasecmp(a,b,c) _strnicmp(a,b,c)

/*
 * Some other compat functions
 */
#ifdef __BORLANDC__
#define _timeb timeb
#define _ftime(a) ftime(a)
#define _strnicmp(a,b,c) strnicmp(a,b,c)
#define _errno errno
#else
#define open(a,b,c) _open(a,b,c)
#define close(a) _close(a)
#define read(a,b,c) _read(a,b,c)
#define write(a,b,c) _write(a,b,c)
#endif

#define popen(a,b) _popen(a,b)
#define pclose(a) _pclose(a)
#define vsnprintf(a,b,c,d) _vsnprintf(a,b,c,d)
#define snprintf _snprintf

#undef EAGAIN					/* doesn't apply on sockets */
#undef EINTR
#define EINTR WSAEINTR
#define EWOULDBLOCK WSAEWOULDBLOCK
#define ECONNRESET WSAECONNRESET
#define EINPROGRESS WSAEINPROGRESS

/*
 * support for handling Windows Socket errors
 */
extern const char *winsock_strerror(int err, char *strerrbuf, size_t buflen);

#endif
