/*-------------------------------------------------------------------------
 *
 * libpq-fs.h--
 *	  definitions for using Inversion file system routines
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpq-fs.h,v 1.7 1998/09/01 04:36:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_FS_H
#define LIBPQ_FS_H

/* UNIX compatibility junk.  This should be in all systems' include files,
   but this is not always the case. */

#ifndef MAXNAMLEN
#define MAXNAMLEN 255
#endif	 /* MAXNAMLEN */

struct pgdirent
{
	unsigned long d_ino;
	unsigned short d_namlen;
	char		d_name[MAXNAMLEN + 1];
};

/*
 * SysV struct dirent doesn't have d_namlen.
 * This counts on d_name being last, which is moderately safe (ha) since
 * it's the variable-length part of the structure.
 */
#ifdef SYSV_DIRENT
#define D_NAMLEN(dp) \
		((dp)->d_reclen - offsetof(struct dirent, d_name[0]))
#else							/* SYSV_DIRENT */
#define D_NAMLEN(dp) \
		((dp)->d_namlen)
#endif	 /* SYSV_DIRENT */

/* for stat(2) */
#ifndef S_IRUSR
/* file modes */

#define S_IRWXU 00700			/* read, write, execute: owner */
#define S_IRUSR 00400			/* read permission: owner */
#define S_IWUSR 00200			/* write permission: owner */
#define S_IXUSR 00100			/* execute permission: owner */

#define S_IRWXG 00070			/* read, write, execute: group */
#define S_IRGRP 00040			/* read permission: group */
#define S_IWGRP 00020			/* write permission: group */
#define S_IXGRP 00010			/* execute permission: group */

#define S_IRWXO 00007			/* read, write, execute: other */
#define S_IROTH 00004			/* read permission: other */
#define S_IWOTH 00002			/* write permission: other */
#define S_IXOTH 00001			/* execute permission: other */

#define _S_IFMT  0170000		/* type of file; sync with S_IFMT */
#define _S_IFBLK 0060000		/* block special; sync with S_IFBLK */
#define _S_IFCHR 0020000		/* character special sync with S_IFCHR */
#define _S_IFDIR 0040000		/* directory; sync with S_IFDIR */
#define _S_IFIFO 0010000		/* FIFO - named pipe; sync with S_IFIFO */
#define _S_IFREG 0100000		/* regular; sync with S_IFREG */

#define S_IFDIR _S_IFDIR
#define S_IFREG _S_IFREG

#define S_ISDIR( mode )			(((mode) & _S_IFMT) == _S_IFDIR)

#endif	 /* S_IRUSR */

/*
 * Inversion doesn't have links.
 */
#ifndef S_ISLNK
#define S_ISLNK(x) 0
#endif

/*
 *	Flags for inversion file system large objects.	Normally, creat()
 *	takes mode arguments, but we don't use them in inversion, since
 *	you get postgres protections.  Instead, we use the low sixteen bits
 *	of the integer mode argument to store the number of the storage
 *	manager to be used, and the high sixteen bits for flags.
 */

#define INV_WRITE		0x00020000
#define INV_READ		0x00040000

/* Error values for p_errno */
#define PEPERM			 1		/* Not owner */
#define PENOENT			 2		/* No such file or directory */
#define PEACCES			 13		/* Permission denied */
#define PEEXIST			 17		/* File exists */
#define PENOTDIR		 20		/* Not a directory */
#define PEISDIR			 21		/* Is a directory */
#define PEINVAL			 22		/* Invalid argument */
#define PENAMETOOLONG	 63		/* File name too long */
#define PENOTEMPTY		 66		/* Directory not empty */
#define PEPGIO			 99		/* postgres backend had problems */

#endif	 /* LIBPQ_FS_H */
