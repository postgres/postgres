/*-------------------------------------------------------------------------
 *
 * port.h
 *	  Header for /port compatibility functions.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/port.h,v 1.39 2004/05/27 14:39:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#if !defined(_MSC_VER) && !defined(__BORLANDC__)
/* for thread.c */
#include <pwd.h>
#include <netdb.h>
#endif
#include <ctype.h>

/* non-blocking */
bool set_noblock(int sock);

/* Portable path handling for Unix/Win32 */
extern char *first_path_separator(const char *filename);
extern char *last_path_separator(const char *filename);
extern void canonicalize_path(char *path);
extern const char *get_progname(const char *argv0);
extern void get_share_path(const char *my_exec_path, char *ret_path);
extern void get_etc_path(const char *my_exec_path, char *ret_path);
extern void get_include_path(const char *my_exec_path, char *ret_path);
extern void get_pkginclude_path(const char *my_exec_path, char *ret_path);
extern void get_pkglib_path(const char *my_exec_path, char *ret_path);
extern void get_locale_path(const char *my_exec_path, char *ret_path);
extern void set_pglocale(const char *argv0, const char *app);

/*
 *	is_absolute_path
 *
 * 	This capability is needed by libpq and initdb.c
 *	On Win32, you can't reference the same object file that is
 *	in two different libraries (pgport and libpq), so a macro is best.
 */
#ifndef WIN32
#define is_absolute_path(filename) \
( \
	((filename)[0] == '/') \
)
#else
#define is_absolute_path(filename) \
( \
	((filename)[0] == '/') || \
	(filename)[0] == '\\' || \
	(isalpha((filename)[0]) && (filename)[1] == ':' && \
	((filename)[2] == '\\' || (filename)[2] == '/')) \
)
#endif





/* Portable way to find binaries */
extern int find_my_exec(const char *argv0, char *retpath);
extern int find_other_exec(const char *argv0, const char *target,
						   const char *versionstr, char *retpath);
#if defined(__CYGWIN__) || defined(WIN32)
#define EXE ".exe"
#define DEVNULL "nul"
#else
#define EXE ""
#define DEVNULL "/dev/null"
#endif

/* Portable delay handling */
extern void pg_usleep(long microsec);

/* Portable SQL-like case-independent comparisons and conversions */
extern int	pg_strcasecmp(const char *s1, const char *s2);
extern int	pg_strncasecmp(const char *s1, const char *s2, size_t n);
extern unsigned char pg_toupper(unsigned char ch);
extern unsigned char pg_tolower(unsigned char ch);

/* Portable prompt handling */
extern char *simple_prompt(const char *prompt, int maxlen, bool echo);

#if defined(bsdi) || defined(netbsd)
extern int	fseeko(FILE *stream, off_t offset, int whence);
extern off_t ftello(FILE *stream);
#endif

/*
 *	WIN32 doesn't allow descriptors returned by pipe() to be used in select(),
 *	so for that platform we use socket() instead of pipe().
 */
#ifndef WIN32
#define pgpipe(a)			pipe(a)
#define piperead(a,b,c)		read(a,b,c)
#define pipewrite(a,b,c)	write(a,b,c)
#else
extern int pgpipe(int handles[2]);
extern int piperead(int s, char* buf, int len);
#define pipewrite(a,b,c)	send(a,b,c,0)

#define PG_SIGNAL_COUNT 32
#define kill(pid,sig)   pgkill(pid,sig)
extern int pgkill(int pid, int sig);
#endif

extern int pclose_check(FILE *stream);

#if defined(__MINGW32__) || defined(__CYGWIN__)
/*
 * Win32 doesn't have reliable rename/unlink during concurrent access
 */
extern int	pgrename(const char *from, const char *to);
extern int	pgunlink(const char *path);
#define rename(from, to)	pgrename(from, to)
#define unlink(path)		pgunlink(path)
#endif

#ifdef WIN32

/* open() replacement to allow delete of held files */
#ifndef _MSC_VER
extern int	win32_open(const char*,int,...);
#define 	open(a,b,...)	win32_open(a,b,##__VA_ARGS__)
#endif

#ifndef __BORLANDC__
#define popen(a,b) _popen(a,b)
#define pclose(a) _pclose(a)
#endif

extern int	copydir(char *fromdir, char *todir);

/* Missing rand functions */
extern long	lrand48(void);
extern void	srand48(long seed);

/* Last parameter not used */
extern int	gettimeofday(struct timeval * tp, struct timezone * tzp);

#else

/*
 *	Win32 requires a special close for sockets and pipes, while on Unix
 *	close() does them all.
 */
#define closesocket close
#endif

/*
 * Default "extern" declarations or macro substitutes for library routines.
 * When necessary, these routines are provided by files in src/port/.
 */
#ifndef HAVE_CRYPT
extern char *crypt(const char *key, const char *setting);
#endif

#ifndef HAVE_FSEEKO
#define fseeko(a, b, c) fseek((a), (b), (c))
#define ftello(a) ftell((a))
#endif

#ifndef HAVE_GETOPT
extern int	getopt(int nargc, char *const * nargv, const char *ostr);
#endif

#ifndef HAVE_ISINF
extern int	isinf(double x);
#endif

#if !defined(HAVE_GETHOSTNAME) && defined(KRB4)
extern int	gethostname(char *name, int namelen);
#endif

#ifndef HAVE_RINT
extern double rint(double x);
#endif

#ifndef HAVE_INET_ATON
#if !defined(_MSC_VER) && !defined(__BORLANDC__)
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
extern int	inet_aton(const char *cp, struct in_addr * addr);
#endif

#ifndef HAVE_STRDUP
extern char *strdup(char const *);
#endif

#ifndef HAVE_RANDOM
extern long random(void);
#endif

#ifndef HAVE_UNSETENV
extern void unsetenv(const char *name);
#endif

#ifndef HAVE_SRANDOM
extern void srandom(unsigned int seed);
#endif

/* thread.h */
extern char *pqStrerror(int errnum, char *strerrbuf, size_t buflen);

#ifndef WIN32
extern int pqGetpwuid(uid_t uid, struct passwd * resultbuf, char *buffer,
		   size_t buflen, struct passwd **result);
#endif

extern int pqGethostbyname(const char *name,
				struct hostent *resultbuf,
				char *buffer, size_t buflen,
				struct hostent **result,
				int *herrno);

/* FIXME: [win32] Placeholder win32 replacements, to allow continued development */
#ifdef WIN32
#define fsync(a)	_commit(a)
#define sync()		_flushall()
#define ftruncate(a,b)	chsize(a,b)
#define WEXITSTATUS(w)  (((w) >> 8) & 0xff)
#define WIFEXITED(w)    (((w) & 0xff) == 0)
#define WIFSIGNALED(w)  (((w) & 0x7f) > 0 && (((w) & 0x7f) < 0x7f))
#define WTERMSIG(w)     ((w) & 0x7f)
#endif
