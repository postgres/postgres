/*
 * $Header: /cvsroot/pgsql/src/bin/pg_dump/pg_backup_tar.h,v 1.4 2002/08/28 20:46:24 momjian Exp $
 *
 * TAR Header
 *
 * Offset	Length	 Contents
 *	 0	  100 bytes  File name ('\0' terminated, 99 maximum length)
 * 100		8 bytes  File mode (in octal ascii)
 * 108		8 bytes  User ID (in octal ascii)
 * 116		8 bytes  Group ID (in octal ascii)
 * 124	   12 bytes  File size (s) (in octal ascii)
 * 136	   12 bytes  Modify time (in octal ascii)
 * 148		8 bytes  Header checksum (in octal ascii)
 * 156		1 bytes  Link flag
 * 157	  100 bytes  Linkname ('\0' terminated, 99 maximum length)
 * 257		8 bytes  Magic ("ustar  \0")
 * 265	   32 bytes  User name ('\0' terminated, 31 maximum length)
 * 297	   32 bytes  Group name ('\0' terminated, 31 maximum length)
 * 329		8 bytes  Major device ID (in octal ascii)
 * 337		8 bytes  Minor device ID (in octal ascii)
 * 345	  167 bytes  Padding
 * 512	 (s+p)bytes  File contents (s+p) := (((s) + 511) & ~511), round up to 512 bytes
 */

/* The linkflag defines the type of file */
#define  LF_OLDNORMAL '\0'		/* Normal disk file, Unix compatible */
#define  LF_NORMAL	  '0'		/* Normal disk file */
#define  LF_LINK	  '1'		/* Link to previously dumped file */
#define  LF_SYMLINK   '2'		/* Symbolic link */
#define  LF_CHR		  '3'		/* Character special file */
#define  LF_BLK		  '4'		/* Block special file */
#define  LF_DIR		  '5'		/* Directory */
#define  LF_FIFO	  '6'		/* FIFO special file */
#define  LF_CONTIG	  '7'		/* Contiguous file */
