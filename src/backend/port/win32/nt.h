typedef char * caddr_t;
typedef unsigned long u_long;
typedef unsigned int u_int;
typedef unsigned short u_short;
typedef unsigned char u_char;
typedef unsigned int mode_t;

typedef u_int uid_t;
typedef u_int gid_t;
typedef int key_t;
#define IPC_PRIVATE ((key_t)0)

/* Common IPC operation flag definitions. We'll use
** the Unix values unless we find a reason not to.
*/
#define IPC_CREAT       0001000         /* create entry if key doesn't exist */
#define IPC_EXCL        0002000         /* fail if key exists */
#define IPC_NOWAIT      0004000         /* error if request must wait */


struct sembuf
{
	u_short	sem_num;
	short	sem_op;
	short	sem_flg;
};

#define USE_POSIX_TIME
#undef HAVE_RINT

#define MAXHOSTNAMELEN	12	/* where is the official definition of this? */
#define MAXPATHLEN _MAX_PATH	/* in winsock.h */

/* NT has stricmp not strcasecmp. Which is ANSI? */
#define strcasecmp(a,b)	_stricmp(a,b)

#define isascii(a)	__isascii(a)

#define random()	rand()

/* These are bogus values used so that we can compile ipc.c */
#define SETALL	2
#define SETVAL	3
#define IPC_RMID 4
#define GETNCNT 5
#define GETVAL 6

