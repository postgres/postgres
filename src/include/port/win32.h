/* $Header: /cvsroot/pgsql/src/include/port/win32.h,v 1.12 2003/08/04 00:43:32 momjian Exp $ */

/* undefine and redefine after #include */
#undef mkdir

#undef ERROR
#include <windows.h>
#undef near

/* Must be here to avoid conflicting with prototype in windows.h */
#define mkdir(a,b)	mkdir(a)


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
#define EIDRM 4096

#define SETALL 8192
#define GETNCNT 16384
#define GETVAL 65536
#define SETVAL 131072
#define GETPID 262144

/*
 *	Shared memory
 */
struct shmid_ds
{
	int			dummy;
	int			shm_nattch;
};

int			shmdt(const void *shmaddr);
void	   *shmat(int memId, void *shmaddr, int flag);
int			shmctl(int shmid, int flag, struct shmid_ds * dummy);
int			shmget(int memKey, int size, int flag);


/*
 *	Semaphores
 */
union semun
{
	int			val;
	struct semid_ds *buf;
	unsigned short *array;
};

struct sembuf
{
	int			sem_flg;
	int			sem_op;
	int			sem_num;
};

int			semctl(int semId, int semNum, int flag, union semun);
int			semget(int semKey, int semNum, int flags);
int			semop(int semId, struct sembuf * sops, int flag);

#define sleep(sec)	(Sleep(sec * 1000), /* no return value */ 0)

/* Some extra signals */
#define SIGHUP				1
#define SIGQUIT				3
#define SIGTRAP				5
#define SIGABRT				22	/* Set to match W32 value -- not UNIX
								 * value */
#define SIGKILL				9
#define SIGPIPE				13
#define SIGALRM				14
#define SIGSTOP				17
#define SIGCONT				19
#define SIGCHLD				20
#define SIGTTIN				21
#define SIGTTOU				22	/* Same as SIGABRT -- no problem, I hope */
#define SIGWINCH			28
#define SIGUSR1				30
#define SIGUSR2				31

struct timezone
{
	int			tz_minuteswest; /* Minutes west of GMT.  */
	int			tz_dsttime;		/* Nonzero if DST is ever in effect.  */
};

/* FROM SRA */

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
