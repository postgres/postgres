/*-------------------------------------------------------------------------
 *
 * port.h
 *	  Header for /port compatibility functions.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port.h,v 1.2 2003/05/16 01:57:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/* Portable path handling for Unix/Win32 */
bool is_absolute_path(const char *filename);
char *first_path_separator(const char *filename);
char *last_path_separator(const char *filename);
char *get_progname(char *argv0);

#if defined(bsdi) || defined(netbsd)
int fseeko(FILE *stream, off_t offset, int whence);
off_t ftello(FILE *stream);
#endif

#ifdef WIN32
/*
 * Win32 doesn't have reliable rename/unlink during concurrent access
 */
#ifndef FRONTEND
int pgrename(const char *from, const char *to);
int pgunlink(const char *path);      
#define rename(from, to)	pgrename(from, to)
#define unlink(path)		pgunlink(path)
#endif

extern int copydir(char *fromdir,char *todir);
extern int gettimeofday(struct timeval *tp, struct timezone *tzp);

#else

/*
 *	Win32 requires a special close for sockets and pipes, while on Unix
 *	close() does them all.
 */
#define	closesocket close
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
extern int getopt(int nargc, char *const *nargv, const char *ostr);
#endif

#ifndef HAVE_ISINF
extern int isinf(double x);
#endif

#if !defined(HAVE_GETHOSTNAME) && defined(KRB4)
extern int gethostname(char *name, int namelen);
#endif

#ifndef HAVE_RINT
extern double rint(double x);
#endif

#ifndef HAVE_INET_ATON
# include <netinet/in.h>
# include <arpa/inet.h>
extern int inet_aton(const char *cp, struct in_addr * addr);
#endif

#ifndef HAVE_STRCASECMP
extern int strcasecmp(char *s1, char *s2);
#endif

#ifndef HAVE_STRDUP
extern char *strdup(char const *);
#endif

#ifndef HAVE_RANDOM
extern long random(void);
#endif

#ifndef HAVE_SRANDOM
extern void srandom(unsigned int seed);
#endif
