/*
 * $PostgreSQL: pgsql/src/include/port/ultrix4.h,v 1.12 2008/05/17 01:28:24 adunstan Exp $ 
 */
#define NOFIXADE
#define NEED_STRDUP

/*
 * Except for those system calls and library functions that are either
 * - covered by the C standard library and Posix.1
 * - or need a declaration to declare parameter or return types,
 * most Ultrix 4 calls are not declared in the system header files.
 * The rest of this header is used to remedy this for PostgreSQL to give a
 * warning-free compilation.
 */

#include <sys/types.h>			/* Declare various types, e.g. size_t, fd_set */

extern int	fp_class_d(double);
extern long random(void);

struct rusage;
extern int	getrusage(int, struct rusage *);

extern int	ioctl(int, unsigned long,...);

extern int	socket(int, int, int);
struct sockaddr;
extern int	connect(int, const struct sockaddr *, int);
typedef int ssize_t;
extern ssize_t send(int, const void *, size_t, int);
extern ssize_t recv(int, void *, size_t, int);
extern int	setsockopt(int, int, int, const void *, int);
extern int	bind(int, const struct sockaddr *, int);
extern int	listen(int, int);
extern int	accept(int, struct sockaddr *, int *);
extern int	getsockname(int, struct sockaddr *, int *);
extern ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *, int *);
extern ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, int);
struct timeval;
extern int	select(int, fd_set *, fd_set *, fd_set *, struct timeval *);

extern int	gethostname(char *, int);

extern int	getopt(int, char *const *, const char *);
extern int	putenv(const char *);

struct itimerval;
extern int	setitimer(int, const struct itimerval *, struct itimerval *);
struct timezone;
extern int	gettimeofday(struct timeval *, struct timezone *);

extern int	fsync(int);
extern int	ftruncate(int, off_t);

extern char *crypt(char *, char *);

/* End of ultrix4.h */
