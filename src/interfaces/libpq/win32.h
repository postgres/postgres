/*
 * src/interfaces/libpq/win32.h
 */
#ifndef __win32_h_included
#define __win32_h_included

/*
 * Some compatibility functions
 */

/* open provided elsewhere */
#define close(a) _close(a)
#define read(a,b,c) _read(a,b,c)
#define write(a,b,c) _write(a,b,c)

#undef EAGAIN					/* doesn't apply on sockets */

/*
 * support for handling Windows Socket errors
 */
extern const char *winsock_strerror(int err, char *strerrbuf, size_t buflen);

#endif
