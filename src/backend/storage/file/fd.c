/*-------------------------------------------------------------------------
 *
 * fd.c--
 *    Virtual file descriptor code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    $Id: fd.c,v 1.9 1996/11/08 05:58:21 momjian Exp $
 *
 * NOTES:
 *
 * This code manages a cache of 'virtual' file descriptors (VFDs).
 * The server opens many file descriptors for a variety of reasons,
 * including base tables, scratch files (e.g., sort and hash spool
 * files), and random calls to C library routines like system(3); it
 * is quite easy to exceed system limits on the number of open files a
 * single process can have.  (This is around 256 on many modern
 * operating systems, but can be as low as 32 on others.)
 *
 * VFDs are managed as an LRU pool, with actual OS file descriptors
 * being opened and closed as needed.  Obviously, if a routine is
 * opened using these interfaces, all subsequent operations must also
 * be through these interfaces (the File type is not a real file
 * descriptor).
 *
 * For this scheme to work, most (if not all) routines throughout the
 * server should use these interfaces instead of calling the C library
 * routines (e.g., open(2) and fopen(3)) themselves.  Otherwise, we
 * may find ourselves short of real file descriptors anyway.
 *
 * This file used to contain a bunch of stuff to support RAID levels 0
 * (jbod), 1 (duplex) and 5 (xor parity).  That stuff is all gone
 * because the parallel query processing code that called it is all
 * gone.  If you really need it you could get it from the original
 * POSTGRES source.
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <sys/file.h>
#include <sys/param.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>

#include "postgres.h"
#include "miscadmin.h"	/* for DataDir */
#include "utils/palloc.h"
#include "storage/fd.h"

#ifdef sparc
/*
 * the SunOS 4 NOFILE is a lie, because the default limit is *not* the
 * maximum number of file descriptors you can have open.
 *
 * we have to either use this number (the default dtablesize) or
 * explicitly call setrlimit(RLIMIT_NOFILE, NOFILE).
 */
#include <sys/user.h>
#undef NOFILE
#define NOFILE NOFILE_IN_U
#endif /* sparc */

#if defined(sparc_solaris) || defined(i386_solaris)
#include <sys/user.h>
#undef NOFILE
#define NOFILE 64
#endif /* sparc_solaris || i386_solaris */

/*
 * Problem: Postgres does a system(ld...) to do dynamic loading.  This
 * will open several extra files in addition to those used by
 * Postgres.  We need to do this hack to guarentee that there are file
 * descriptors free for ld to use.
 *
 * The current solution is to limit the number of files descriptors
 * that this code will allocated at one time.  (it leaves
 * RESERVE_FOR_LD free).
 *
 * (Even though most dynamic loaders now use dlopen(3) or the
 * equivalent, the OS must still open several files to perform the
 * dynamic loading.  Keep this here.)
 */
#define RESERVE_FOR_LD	10

/*
 * If we are using weird storage managers, we may need to keep real
 * file descriptors open so that the jukebox server doesn't think we
 * have gone away (and no longer care about a platter or file that
 * we've been using).  This might be an actual file descriptor for a
 * local jukebox interface that uses paths, or a socket connection for
 * a network jukebox server.  Since we can't be opening and closing
 * these descriptors at whim, we must make allowances for them.
 */
#ifdef HP_JUKEBOX
#define RESERVE_FOR_JB	25
#define	MAXFILES	((NOFILE - RESERVE_FOR_LD) - RESERVE_FOR_JB)
#else /* HP_JUKEBOX */
#define	MAXFILES	(NOFILE - RESERVE_FOR_LD)
#endif /* HP_JUKEBOX */

/* Debugging.... */

#ifdef FDDEBUG
# define DO_DB(A) A
#else
# define DO_DB(A) /* A */
#endif

#define VFD_CLOSED -1

#include "storage/fd.h"
#include "utils/elog.h"

#define FileIsNotOpen(file) (VfdCache[file].fd == VFD_CLOSED)

typedef struct vfd {
    signed short	fd;
    unsigned short	fdstate;

#define FD_DIRTY	(1 << 0)

    File	nextFree;
    File	lruMoreRecently;
    File	lruLessRecently;
    long	seekPos;
    char	*fileName;
    int		fileFlags;
    int		fileMode;
} Vfd;

/*
 * Virtual File Descriptor array pointer and size.  This grows as
 * needed.
 */
static	Vfd	*VfdCache;
static	Size	SizeVfdCache = 0;

/*
 * Minimum number of file descriptors known to be free.
 */
static	int	FreeFd = 0;

/*
 * Number of file descriptors known to be open.
 */
static	int	nfile = 0;

/*
 * we use the name of the null device in various places, mostly so
 * that we can open it and find out if we really have any descriptors
 * available or not.
 */
#ifndef WIN32
static char *Nulldev = "/dev/null";
static char Sep_char = '/';
#else
static char *Nulldev = "NUL";
static char Sep_char = '\\';
#endif /* WIN32 */

/*
 * Private Routines
 *
 * Delete	   - delete a file from the Lru ring
 * LruDelete	   - remove a file from the Lru ring and close
 * Insert	   - put a file at the front of the Lru ring
 * LruInsert	   - put a file at the front of the Lru ring and open
 * AssertLruRoom   - make sure that there is a free fd.
 *
 * the Last Recently Used ring is a doubly linked list that begins and
 * ends on element zero.
 *
 * example:
 *
 *     /--less----\                /---------\
 *     v           \              v           \
 *   #0 --more---> LeastRecentlyUsed --more-\ \
 *    ^\                                    | |
 *     \\less--> MostRecentlyUsedFile   <---/ |
 *      \more---/                    \--less--/
 *
 * AllocateVfd	   - grab a free (or new) file record (from VfdArray)
 * FreeVfd	   - free a file record
 *
 */
static void Delete(File file);
static void LruDelete(File file);
static void Insert(File file);
static int LruInsert (File file);
static void AssertLruRoom(void);
static File AllocateVfd(void);
static void FreeVfd(File file);

static int FileAccess(File file);
static File fileNameOpenFile(FileName fileName, int fileFlags, int fileMode);
static char *filepath(char *filename);

int
pg_fsync(int fd)
{
    extern int fsyncOff;
    return fsyncOff ? 0 : fsync(fd);
}
#define fsync pg_fsync

#if defined(FDDEBUG)
static void
_dump_lru()
{
    int mru = VfdCache[0].lruLessRecently;
    Vfd *vfdP = &VfdCache[mru];
    
    printf("MOST %d ", mru);
    while (mru != 0)
	{
	    mru = vfdP->lruLessRecently;
	    vfdP = &VfdCache[mru];
	    printf("%d ", mru);
	}
    printf("LEAST\n");
}
#endif /* FDDEBUG */

static void
Delete(File file)
{
    Vfd	*fileP;
    
    DO_DB(printf("DEBUG:	Delete %d (%s)\n",
		 file, VfdCache[file].fileName));
    DO_DB(_dump_lru());
    
    Assert(file != 0);
    
    fileP = &VfdCache[file];

    VfdCache[fileP->lruLessRecently].lruMoreRecently =
	VfdCache[file].lruMoreRecently;
    VfdCache[fileP->lruMoreRecently].lruLessRecently =
	VfdCache[file].lruLessRecently;
    
    DO_DB(_dump_lru());
}

static void
LruDelete(File file)
{
    Vfd     *fileP;
    int	returnValue;
    
    DO_DB(printf("DEBUG:	LruDelete %d (%s)\n",
		 file, VfdCache[file].fileName));
    
    Assert(file != 0);
    
    fileP = &VfdCache[file];
    
    /* delete the vfd record from the LRU ring */
    Delete(file);
    
    /* save the seek position */
    fileP->seekPos = (long) lseek(fileP->fd, 0L, SEEK_CUR);
    Assert( fileP->seekPos != -1);
    
    /* if we have written to the file, sync it */
    if (fileP->fdstate & FD_DIRTY) {
	returnValue = fsync(fileP->fd);
	Assert(returnValue != -1);
	fileP->fdstate &= ~FD_DIRTY;
    }
    
    /* close the file */
    returnValue = close(fileP->fd);
    Assert(returnValue != -1);
    
    --nfile;
    fileP->fd = VFD_CLOSED;
    
    /* note that there is now one more free real file descriptor */
    FreeFd++;
}

static void
Insert(File file)
{
    Vfd	*vfdP;
    
    DO_DB(printf("DEBUG:	Insert %d (%s)\n",
		 file, VfdCache[file].fileName));
    DO_DB(_dump_lru());
    
    vfdP = &VfdCache[file];
    
    vfdP->lruMoreRecently = 0;
    vfdP->lruLessRecently = VfdCache[0].lruLessRecently;
    VfdCache[0].lruLessRecently = file;
    VfdCache[vfdP->lruLessRecently].lruMoreRecently = file;
    
    DO_DB(_dump_lru());
}

static int
LruInsert (File file)
{
    Vfd	*vfdP;
    int	returnValue;
    
    DO_DB(printf("DEBUG:	LruInsert %d (%s)\n",
		 file, VfdCache[file].fileName));
    
    vfdP = &VfdCache[file];
    
    if (FileIsNotOpen(file)) {
	int tmpfd;
	
        /*
	 * Note, we check to see if there's a free file descriptor
	 * before attempting to open a file. One general way to do
	 * this is to try to open the null device which everybody
	 * should be able to open all the time. If this fails, we
	 * assume this is because there's no free file descriptors.
	 */
    tryAgain:
	tmpfd = open(Nulldev, O_CREAT|O_RDWR, 0666);
	if (tmpfd < 0) {
	    FreeFd = 0;
	    errno = 0;
	    AssertLruRoom();
	    goto tryAgain;
	} else {
	    close(tmpfd);
	}
	vfdP->fd = open(vfdP->fileName,vfdP->fileFlags,vfdP->fileMode);
	
	if (vfdP->fd < 0) {
	    DO_DB(printf("RE_OPEN FAILED: %d\n",
			 errno));
	    return (vfdP->fd);
	} else {
	    DO_DB(printf("RE_OPEN SUCCESS\n"));
	    ++nfile;
	}
	
	/* seek to the right position */
	if (vfdP->seekPos != 0L) {
	    returnValue =
		lseek(vfdP->fd, vfdP->seekPos, SEEK_SET);
	    Assert(returnValue != -1);
	}
	
	/* init state on open */
	vfdP->fdstate = 0x0;
	
	/* note that a file descriptor has been used up */
	if (FreeFd > 0)
	    FreeFd--;
    }
    
    /*
     * put it at the head of the Lru ring
     */
    
    Insert(file);
    
    return (0);
}

static void
AssertLruRoom()
{
    DO_DB(printf("DEBUG:	AssertLruRoom (FreeFd = %d)\n",
		 FreeFd));
    
    if (FreeFd <= 0 || nfile >= MAXFILES) {
	LruDelete(VfdCache[0].lruMoreRecently);
    }
}

static File
AllocateVfd()
{
    Index	i;
    File	file;
    
    DO_DB(printf("DEBUG:	AllocateVfd\n"));
    
    if (SizeVfdCache == 0) {
	
	/* initialize */
	VfdCache = (Vfd *)malloc(sizeof(Vfd));
	
	VfdCache->nextFree = 0;
	VfdCache->lruMoreRecently = 0;
	VfdCache->lruLessRecently = 0;
	VfdCache->fd = VFD_CLOSED;
	VfdCache->fdstate = 0x0;
	
	SizeVfdCache = 1;
    }
    
    if (VfdCache[0].nextFree == 0) {
	
	/*
	 * The free list is empty so it is time to increase the
	 * size of the array
	 */
	
	VfdCache =(Vfd *)realloc(VfdCache, sizeof(Vfd)*SizeVfdCache*2);
	Assert(VfdCache != NULL);
	
	/*
	 * Set up the free list for the new entries
	 */
	
	for (i = SizeVfdCache; i < 2*SizeVfdCache; i++)  {
	    memset((char *) &(VfdCache[i]), 0, sizeof(VfdCache[0]));
	    VfdCache[i].nextFree = i+1;
	    VfdCache[i].fd = VFD_CLOSED;
	}
	
	/*
	 * Element 0 is the first and last element of the free
	 * list
	 */
	
	VfdCache[0].nextFree = SizeVfdCache;
	VfdCache[2*SizeVfdCache-1].nextFree = 0;
	
	/*
	 * Record the new size
	 */
	
	SizeVfdCache *= 2;
    }
    file = VfdCache[0].nextFree;
    
    VfdCache[0].nextFree = VfdCache[file].nextFree;
    
    return file;
}

static void
FreeVfd(File file)
{
    DO_DB(printf("DB: FreeVfd: %d (%s)\n",
		 file, VfdCache[file].fileName));
    
    VfdCache[file].nextFree = VfdCache[0].nextFree;
    VfdCache[0].nextFree = file;
}

static char *
filepath(char *filename)
{
    char *buf;
    char basename[16];
    int len;

#ifndef WIN32    
    if (*filename != Sep_char) {
#else
    if (!(filename[1] == ':' && filename[2] == Sep_char)) {
#endif /* WIN32 */	

	/* Either /base/ or \base\ */
	sprintf(basename, "%cbase%c", Sep_char, Sep_char);

	len = strlen(DataDir) + strlen(basename) + strlen(GetDatabaseName())
	    + strlen(filename) + 2;
	buf = (char*) palloc(len);
	sprintf(buf, "%s%s%s%c%s",
		DataDir, basename, GetDatabaseName(), Sep_char, filename);
    } else {
	buf = (char *) palloc(strlen(filename) + 1);
	strcpy(buf, filename);
    }
    
    return(buf);
}

static int
FileAccess(File file)
{
    int	returnValue;
    
    DO_DB(printf("DB: FileAccess %d (%s)\n",
		 file, VfdCache[file].fileName));
    
    /*
     * Is the file open?  If not, close the least recently used,
     * then open it and stick it at the head of the used ring
     */
    
    if (FileIsNotOpen(file)) {
	
	AssertLruRoom();
	
	returnValue = LruInsert(file);
	if (returnValue != 0)
	    return returnValue;
	
    } else {
	
	/*
	 * We now know that the file is open and that it is not the
	 * last one accessed, so we need to more it to the head of
	 * the Lru ring.
	 */
	
	Delete(file);
	Insert(file);
    }
    
    return (0);
}

/*
 *  Called when we get a shared invalidation message on some relation.
 */
void
FileInvalidate(File file)
{
    if (!FileIsNotOpen(file)) {
	LruDelete(file);
    }
}

/* VARARGS2 */
static File
fileNameOpenFile(FileName fileName,
		 int fileFlags,
		 int fileMode)
{
    static int osRanOut = 0;
    File	file;
    Vfd	*vfdP;
    int     tmpfd;
    
    DO_DB(printf("DEBUG: FileNameOpenFile: %s %x %o\n",
		 fileName, fileFlags, fileMode));
    
    file = AllocateVfd();
    vfdP = &VfdCache[file];
    
    if (nfile >= MAXFILES || (FreeFd == 0 && osRanOut)) {
	AssertLruRoom();
    }
    
 tryAgain:
    tmpfd = open(Nulldev, O_CREAT|O_RDWR, 0666);
    if (tmpfd < 0) {
	DO_DB(printf("DB: not enough descs, retry, er= %d\n",
		     errno));
	errno = 0;
	FreeFd = 0;
	osRanOut = 1;
	AssertLruRoom();
	goto tryAgain;
    } else {
	close(tmpfd);
    }
    
#ifdef WIN32
      fileFlags |= _O_BINARY;
#endif /* WIN32 */
    vfdP->fd = open(fileName,fileFlags,fileMode);
    vfdP->fdstate = 0x0;
    
    if (vfdP->fd < 0) {
	FreeVfd(file);
	return -1;
    }
    ++nfile;
    DO_DB(printf("DB: FNOF success %d\n",
		 vfdP->fd));
    
    (void)LruInsert(file);
    
    if (fileName==NULL) {
	elog(WARN, "fileNameOpenFile: NULL fname");
    }
    vfdP->fileName = malloc(strlen(fileName)+1);
    strcpy(vfdP->fileName,fileName);
    
    vfdP->fileFlags = fileFlags & ~(O_TRUNC|O_EXCL);
    vfdP->fileMode = fileMode;
    vfdP->seekPos = 0;
    
    return file;
}

/*
 * open a file in the database directory ($PGDATA/base/...)
 */
File
FileNameOpenFile(FileName fileName, int fileFlags, int fileMode)
{
    File fd;
    char *fname;
    
    fname = filepath(fileName);
    fd = fileNameOpenFile(fname, fileFlags, fileMode);
    pfree(fname);
    return(fd);
}

/*
 * open a file in an arbitrary directory
 */
File
PathNameOpenFile(FileName fileName, int fileFlags, int fileMode)
{
    return(fileNameOpenFile(fileName, fileFlags, fileMode));
}

void
FileClose(File file)
{
    int	returnValue;
    
    DO_DB(printf("DEBUG: FileClose: %d (%s)\n",
		 file, VfdCache[file].fileName));
    
    if (!FileIsNotOpen(file)) {
	
	/* remove the file from the lru ring */
	Delete(file);
	
	/* record the new free operating system file descriptor */
	FreeFd++;
	
	/* if we did any writes, sync the file before closing */
	if (VfdCache[file].fdstate & FD_DIRTY) {
	    returnValue = fsync(VfdCache[file].fd);
	    Assert(returnValue != -1);
	    VfdCache[file].fdstate &= ~FD_DIRTY;
	}
	
	/* close the file */
	returnValue = close(VfdCache[file].fd);
	Assert(returnValue != -1);
	
	--nfile;
	VfdCache[file].fd = VFD_CLOSED;
    }
    /*
     * Add the Vfd slot to the free list
     */
    FreeVfd(file);
    /*
     * Free the filename string
     */
    free(VfdCache[file].fileName);
}

void
FileUnlink(File file)
{
    int returnValue;
    
    DO_DB(printf("DB: FileClose: %d (%s)\n",
		 file, VfdCache[file].fileName));
    
    if (!FileIsNotOpen(file)) {
	
	/* remove the file from the lru ring */
	Delete(file);
	
	/* record the new free operating system file descriptor */
	FreeFd++;
	
	/* if we did any writes, sync the file before closing */
	if (VfdCache[file].fdstate & FD_DIRTY) {
	    returnValue = fsync(VfdCache[file].fd);
	    Assert(returnValue != -1);
	    VfdCache[file].fdstate &= ~FD_DIRTY;
	}
	
	/* close the file */
	returnValue = close(VfdCache[file].fd);
	Assert(returnValue != -1);
	
	--nfile;
	VfdCache[file].fd = VFD_CLOSED;
    }
    /* add the Vfd slot to the free list */
    FreeVfd(file);
    
    /* free the filename string */
    unlink(VfdCache[file].fileName);
    free(VfdCache[file].fileName);
}

int
FileRead(File file, char *buffer, int amount)
{
    int	returnCode;

    DO_DB(printf("DEBUG: FileRead: %d (%s) %d 0x%x\n",
		 file, VfdCache[file].fileName, amount, buffer));
    
    FileAccess(file);
    returnCode = read(VfdCache[file].fd, buffer, amount);
    if (returnCode > 0) {
	VfdCache[file].seekPos += returnCode;
    }
    
    return returnCode;
}

int
FileWrite(File file, char *buffer, int amount)
{
    int	returnCode;

    DO_DB(printf("DB: FileWrite: %d (%s) %d 0x%lx\n",
		 file, VfdCache[file].fileName, amount, buffer));
    
    FileAccess(file);
    returnCode = write(VfdCache[file].fd, buffer, amount);
    if (returnCode > 0) {  /* changed by Boris with Mao's advice */
	VfdCache[file].seekPos += returnCode;
    }
    
    /* record the write */
    VfdCache[file].fdstate |= FD_DIRTY;
    
    return returnCode;
}

long
FileSeek(File file, long offset, int whence)
{
    int	returnCode;
    
    DO_DB(printf("DEBUG: FileSeek: %d (%s) %d %d\n",
		 file, VfdCache[file].fileName, offset, whence));
    
    if (FileIsNotOpen(file)) {
	switch(whence) {
	case SEEK_SET:
	    VfdCache[file].seekPos = offset;
	    return offset;
	case SEEK_CUR:
	    VfdCache[file].seekPos = VfdCache[file].seekPos +offset;
	    return VfdCache[file].seekPos;
	case SEEK_END:
	    FileAccess(file);
	    returnCode = VfdCache[file].seekPos = 
		lseek(VfdCache[file].fd, offset, whence);
	    return returnCode;
	default:
	    elog(WARN, "FileSeek: invalid whence: %d", whence);
	    break;
	}
    } else {
	returnCode = VfdCache[file].seekPos = 
	    lseek(VfdCache[file].fd, offset, whence);
	return returnCode;
    }
    /*NOTREACHED*/
    return(-1L);
}

/*
 * XXX not actually used but here for completeness
 */
long
FileTell(File file)
{
    DO_DB(printf("DEBUG: FileTell %d (%s)\n",
		 file, VfdCache[file].fileName));
    return VfdCache[file].seekPos;
}

int
FileTruncate(File file, int offset)
{
    int returnCode;

    DO_DB(printf("DEBUG: FileTruncate %d (%s)\n",
		 file, VfdCache[file].fileName));
    
    (void) FileSync(file);
    (void) FileAccess(file);
    returnCode = ftruncate(VfdCache[file].fd, offset);
    return(returnCode);
}

int
FileSync(File file)
{
    int	returnCode;
    
    /*
     *  If the file isn't open, then we don't need to sync it; we
     *  always sync files when we close them.  Also, if we haven't
     *  done any writes that we haven't already synced, we can ignore
     *  the request.
     */
    
    if (VfdCache[file].fd < 0 || !(VfdCache[file].fdstate & FD_DIRTY)) {
	returnCode = 0;
    } else {
	returnCode = fsync(VfdCache[file].fd);
	VfdCache[file].fdstate &= ~FD_DIRTY;
    }
    
    return returnCode;
}

int
FileNameUnlink(char *filename)
{
    int retval;
    char *fname;

    fname = filepath(filename);
    retval = unlink(fname);
    pfree(fname);
    return(retval);
}

/*
 * if we want to be sure that we have a real file descriptor available
 * (e.g., we want to know this in psort) we call AllocateFile to force
 * availability.  when we are done we call FreeFile to deallocate the
 * descriptor.
 *
 * allocatedFiles keeps track of how many have been allocated so we
 * can give a warning if there are too few left.
 */
static int allocatedFiles = 0;

void
AllocateFile()
{
    int fd;
    int fdleft;

    while ((fd = open(Nulldev,O_WRONLY,0)) < 0) {
	if (errno == EMFILE) {
	    errno = 0;
	    FreeFd = 0;
	    AssertLruRoom();
	} else {
	    elog(WARN,"Open: %s in %s line %d\n", Nulldev,
		 __FILE__, __LINE__);
	}
    }
    close(fd);
    ++allocatedFiles;
    fdleft = MAXFILES - allocatedFiles;
    if (fdleft < 6) {
	elog(DEBUG,"warning: few usable file descriptors left (%d)", fdleft);
    }
    
    DO_DB(printf("DEBUG: AllocatedFile.  FreeFd = %d\n",
		 FreeFd));
}

/*
 * XXX What happens if FreeFile() is called without a previous
 * AllocateFile()?
 */
void
FreeFile()
{
    DO_DB(printf("DEBUG: FreeFile.  FreeFd now %d\n",
		 FreeFd));
    FreeFd++;
    nfile++;			/* dangerous */
    Assert(allocatedFiles > 0);
    --allocatedFiles;
}

void
closeAllVfds()
{
    int i;
    for (i=0; i<SizeVfdCache; i++) {
	if (!FileIsNotOpen(i))
	    LruDelete(i);
    }
}

void
closeOneVfd()
{
    int tmpfd;
    
    tmpfd = open(Nulldev, O_CREAT | O_RDWR, 0666);
    if (tmpfd < 0) {
	FreeFd = 0;
	AssertLruRoom();
	FreeFd = 0;
    }
    else
	close(tmpfd);
}
