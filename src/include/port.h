/*-------------------------------------------------------------------------
 *
 * port.h
 *	  Header for src/port/ compatibility functions.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/port.h,v 1.116 2008/01/01 19:45:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PORT_H
#define PG_PORT_H

#include <ctype.h>
#include <netdb.h>
#include <pwd.h>

/* non-blocking */
extern bool pg_set_noblock(int sock);
extern bool pg_set_block(int sock);

/* Portable path handling for Unix/Win32 (in path.c) */

extern char *first_dir_separator(const char *filename);
extern char *last_dir_separator(const char *filename);
extern char *first_path_separator(const char *pathlist);
extern void join_path_components(char *ret_path,
					 const char *head, const char *tail);
extern void canonicalize_path(char *path);
extern void make_native_path(char *path);
extern bool path_contains_parent_reference(const char *path);
extern bool path_is_prefix_of_path(const char *path1, const char *path2);
extern const char *get_progname(const char *argv0);
extern void get_share_path(const char *my_exec_path, char *ret_path);
extern void get_etc_path(const char *my_exec_path, char *ret_path);
extern void get_include_path(const char *my_exec_path, char *ret_path);
extern void get_pkginclude_path(const char *my_exec_path, char *ret_path);
extern void get_includeserver_path(const char *my_exec_path, char *ret_path);
extern void get_lib_path(const char *my_exec_path, char *ret_path);
extern void get_pkglib_path(const char *my_exec_path, char *ret_path);
extern void get_locale_path(const char *my_exec_path, char *ret_path);
extern void get_doc_path(const char *my_exec_path, char *ret_path);
extern void get_man_path(const char *my_exec_path, char *ret_path);
extern bool get_home_path(char *ret_path);
extern void get_parent_directory(char *path);

/* port/dirmod.c */
extern char **pgfnames(char *path);
extern void pgfnames_cleanup(char **filenames);

/*
 *	is_absolute_path
 *
 *	By making this a macro we avoid needing to include path.c in libpq.
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
	(isalpha((unsigned char) ((filename)[0])) && (filename)[1] == ':' && \
	((filename)[2] == '\\' || (filename)[2] == '/')) \
)
#endif

/* Portable locale initialization (in exec.c) */
extern void set_pglocale_pgservice(const char *argv0, const char *app);

/* Portable way to find binaries (in exec.c) */
extern int	find_my_exec(const char *argv0, char *retpath);
extern int find_other_exec(const char *argv0, const char *target,
				const char *versionstr, char *retpath);

#if defined(WIN32) || defined(__CYGWIN__)
#define EXE ".exe"
#else
#define EXE ""
#endif

#if defined(WIN32) && !defined(__CYGWIN__)
#define DEVNULL "nul"
/* "con" does not work from the Msys 1.0.10 console (part of MinGW). */
#define DEVTTY	"con"
#else
#define DEVNULL "/dev/null"
#define DEVTTY "/dev/tty"
#endif

/*
 *	Win32 needs double quotes at the beginning and end of system()
 *	strings.  If not, it gets confused with multiple quoted strings.
 *	It also requires double-quotes around the executable name and
 *	any files used for redirection.  Other args can use single-quotes.
 *
 *	Generated using Win32 "CMD /?":
 *
 *	1. If all of the following conditions are met, then quote characters
 *	on the command line are preserved:
 *
 *	 - no /S switch
 *	 - exactly two quote characters
 *	 - no special characters between the two quote characters, where special
 *	   is one of: &<>()@^|
 *	 - there are one or more whitespace characters between the the two quote
 *	   characters
 *	 - the string between the two quote characters is the name of an
 *	   executable file.
 *
 *	 2. Otherwise, old behavior is to see if the first character is a quote
 *	 character and if so, strip the leading character and remove the last
 *	 quote character on the command line, preserving any text after the last
 *	 quote character.
 */
#if defined(WIN32) && !defined(__CYGWIN__)
#define SYSTEMQUOTE "\""
#else
#define SYSTEMQUOTE ""
#endif

/* Portable delay handling */
extern void pg_usleep(long microsec);

/* Portable SQL-like case-independent comparisons and conversions */
extern int	pg_strcasecmp(const char *s1, const char *s2);
extern int	pg_strncasecmp(const char *s1, const char *s2, size_t n);
extern unsigned char pg_toupper(unsigned char ch);
extern unsigned char pg_tolower(unsigned char ch);

#ifdef USE_REPL_SNPRINTF

/*
 * Versions of libintl >= 0.13 try to replace printf() and friends with
 * macros to their own versions that understand the %$ format.	We do the
 * same, so disable their macros, if they exist.
 */
#ifdef vsnprintf
#undef vsnprintf
#endif
#ifdef snprintf
#undef snprintf
#endif
#ifdef sprintf
#undef sprintf
#endif
#ifdef vfprintf
#undef vfprintf
#endif
#ifdef fprintf
#undef fprintf
#endif
#ifdef printf
#undef printf
#endif

extern int	pg_vsnprintf(char *str, size_t count, const char *fmt, va_list args);
extern int
pg_snprintf(char *str, size_t count, const char *fmt,...)
/* This extension allows gcc to check the format string */
__attribute__((format(printf, 3, 4)));
extern int
pg_sprintf(char *str, const char *fmt,...)
/* This extension allows gcc to check the format string */
__attribute__((format(printf, 2, 3)));
extern int	pg_vfprintf(FILE *stream, const char *fmt, va_list args);
extern int
pg_fprintf(FILE *stream, const char *fmt,...)
/* This extension allows gcc to check the format string */
__attribute__((format(printf, 2, 3)));
extern int
pg_printf(const char *fmt,...)
/* This extension allows gcc to check the format string */
__attribute__((format(printf, 1, 2)));

/*
 *	The GCC-specific code below prevents the __attribute__(... 'printf')
 *	above from being replaced, and this is required because gcc doesn't
 *	know anything about pg_printf.
 */
#ifdef __GNUC__
#define vsnprintf(...)	pg_vsnprintf(__VA_ARGS__)
#define snprintf(...)	pg_snprintf(__VA_ARGS__)
#define sprintf(...)	pg_sprintf(__VA_ARGS__)
#define vfprintf(...)	pg_vfprintf(__VA_ARGS__)
#define fprintf(...)	pg_fprintf(__VA_ARGS__)
#define printf(...)		pg_printf(__VA_ARGS__)
#else
#define vsnprintf		pg_vsnprintf
#define snprintf		pg_snprintf
#define sprintf			pg_sprintf
#define vfprintf		pg_vfprintf
#define fprintf			pg_fprintf
#define printf			pg_printf
#endif
#endif   /* USE_REPL_SNPRINTF */

/* Portable prompt handling */
extern char *simple_prompt(const char *prompt, int maxlen, bool echo);

/*
 *	WIN32 doesn't allow descriptors returned by pipe() to be used in select(),
 *	so for that platform we use socket() instead of pipe().
 *	There is some inconsistency here because sometimes we require pg*, like
 *	pgpipe, but in other cases we define rename to pgrename just on Win32.
 */
#ifndef WIN32
/*
 *	The function prototypes are not supplied because every C file
 *	includes this file.
 */
#define pgpipe(a)			pipe(a)
#define piperead(a,b,c)		read(a,b,c)
#define pipewrite(a,b,c)	write(a,b,c)
#else
extern int	pgpipe(int handles[2]);
extern int	piperead(int s, char *buf, int len);

#define pipewrite(a,b,c)	send(a,b,c,0)

#define PG_SIGNAL_COUNT 32
#define kill(pid,sig)	pgkill(pid,sig)
extern int	pgkill(int pid, int sig);
#endif

extern int	pclose_check(FILE *stream);

/* Global variable holding time zone information. */
#ifndef __CYGWIN__
#define TIMEZONE_GLOBAL timezone
#define TZNAME_GLOBAL tzname
#else
#define TIMEZONE_GLOBAL _timezone
#define TZNAME_GLOBAL _tzname
#endif

#if defined(WIN32) || defined(__CYGWIN__)
/*
 *	Win32 doesn't have reliable rename/unlink during concurrent access.
 */
extern int	pgrename(const char *from, const char *to);
extern int	pgunlink(const char *path);

/* Include this first so later includes don't see these defines */
#ifdef WIN32_ONLY_COMPILER
#include <io.h>
#endif

#define rename(from, to)		pgrename(from, to)
#define unlink(path)			pgunlink(path)
#endif   /* defined(WIN32) || defined(__CYGWIN__) */

/*
 *	Win32 also doesn't have symlinks, but we can emulate them with
 *	junction points on newer Win32 versions.
 *
 *	Cygwin has its own symlinks which work on Win95/98/ME where
 *	junction points don't, so use those instead.  We have no way of
 *	knowing what type of system Cygwin binaries will be run on.
 *		Note: Some CYGWIN includes might #define WIN32.
 */
#if defined(WIN32) && !defined(__CYGWIN__)
extern int	pgsymlink(const char *oldpath, const char *newpath);

#define symlink(oldpath, newpath)	pgsymlink(oldpath, newpath)
#endif

extern void copydir(char *fromdir, char *todir, bool recurse);

extern bool rmtree(char *path, bool rmtopdir);

#if defined(WIN32) && !defined(__CYGWIN__)

/* open() and fopen() replacements to allow deletion of open files and
 * passing of other special options.
 */
#define		O_DIRECT	0x80000000
extern int	pgwin32_open(const char *, int,...);
extern FILE *pgwin32_fopen(const char *, const char *);

#ifndef FRONTEND
#define		open(a,b,c) pgwin32_open(a,b,c)
#define		fopen(a,b) pgwin32_fopen(a,b)
#endif

#define popen(a,b) _popen(a,b)
#define pclose(a) _pclose(a)

/* Missing rand functions */
extern long lrand48(void);
extern void srand48(long seed);

/* New versions of MingW have gettimeofday, old mingw and msvc don't */
#ifndef HAVE_GETTIMEOFDAY
/* Last parameter not used */
extern int	gettimeofday(struct timeval * tp, struct timezone * tzp);
#endif
#else							/* !WIN32 */

/*
 *	Win32 requires a special close for sockets and pipes, while on Unix
 *	close() does them all.
 */
#define closesocket close
#endif   /* WIN32 */

/*
 * Default "extern" declarations or macro substitutes for library routines.
 * When necessary, these routines are provided by files in src/port/.
 */
#ifndef HAVE_CRYPT
extern char *crypt(const char *key, const char *setting);
#endif

#if defined(bsdi) || defined(netbsd)
extern int	fseeko(FILE *stream, off_t offset, int whence);
extern off_t ftello(FILE *stream);
#endif

#ifndef HAVE_FSEEKO
#define fseeko(a, b, c) fseek(a, b, c)
#define ftello(a)		ftell(a)
#endif

#ifndef HAVE_GETOPT
extern int	getopt(int nargc, char *const * nargv, const char *ostr);
#endif

#ifndef HAVE_ISINF
extern int	isinf(double x);
#endif

#ifndef HAVE_RINT
extern double rint(double x);
#endif

#ifndef HAVE_INET_ATON
#include <netinet/in.h>
#include <arpa/inet.h>
extern int	inet_aton(const char *cp, struct in_addr * addr);
#endif

#ifndef HAVE_STRDUP
extern char *strdup(const char *str);
#endif

#if !HAVE_DECL_STRLCAT
extern size_t strlcat(char *dst, const char *src, size_t siz);
#endif

#if !HAVE_DECL_STRLCPY
extern size_t strlcpy(char *dst, const char *src, size_t siz);
#endif

#if !defined(HAVE_RANDOM) && !defined(__BORLANDC__)
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

#if !defined(WIN32) || defined(__CYGWIN__)
extern int pqGetpwuid(uid_t uid, struct passwd * resultbuf, char *buffer,
		   size_t buflen, struct passwd ** result);
#endif

extern int pqGethostbyname(const char *name,
				struct hostent * resultbuf,
				char *buffer, size_t buflen,
				struct hostent ** result,
				int *herrno);

extern void pg_qsort(void *base, size_t nel, size_t elsize,
		 int (*cmp) (const void *, const void *));

#define qsort(a,b,c,d) pg_qsort(a,b,c,d)

typedef int (*qsort_arg_comparator) (const void *a, const void *b, void *arg);

extern void qsort_arg(void *base, size_t nel, size_t elsize,
		  qsort_arg_comparator cmp, void *arg);

/* port/chklocale.c */
extern int	pg_get_encoding_from_locale(const char *ctype);

#endif   /* PG_PORT_H */
