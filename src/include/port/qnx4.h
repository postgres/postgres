#include <sys/types.h>			/* for namser.h */
#include <arpa/nameser.h>		/* for BYTE_ORDER */
#include <process.h>			/* for execv */
#include <ioctl.h>				/* for unix.h */
#include <unix.h>
#include <sys/select.h>			/* for select */

#undef HAVE_GETRUSAGE

extern int	isnan(double dsrc);
extern long random(void);
extern void srandom(unsigned int seed);
