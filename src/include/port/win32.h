/* $Header: /cvsroot/pgsql/src/include/port/win32.h,v 1.8 2003/04/24 21:23:01 momjian Exp $ */

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
 *	IPC defines
 */
#define IPC_RMID 256
#define IPC_CREAT 512
#define IPC_EXCL 1024
#define IPC_PRIVATE 234564
#define IPC_NOWAIT	2048
#define IPC_STAT 4096


/*
 *	Shared memory
 */
struct shmid_ds
{
	int		dummy;
	int		shm_nattch;
};

int   shmdt(const void *shmaddr);
void* shmat(int memId, void* shmaddr, int flag);
int   shmctl(int shmid, int flag, struct shmid_ds * dummy);
int   shmget(int memKey, int size, int flag);


/*
 *	Semaphores
 */
union semun
{
	int 		val;
	struct semid_ds *buf;
	unsigned short *array;
};

struct sembuf
{
	int 		sem_flg;
	int 		sem_op;
	int 		sem_num;
};

int	  semctl(int semId, int semNum, int flag, union semun);
int	  semget(int semKey, int semNum, int flags);
int	  semop(int semId, struct sembuf * sops, int flag);


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


