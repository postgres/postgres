/*-------------------------------------------------------------------------
 *
 * dirmod.c
 *	  rename/unlink()
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	These are replacement versions of unlink and rename that work on
 *	Win32 (NT, Win2k, XP).	replace() doesn't work on Win95/98/Me.
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/dirmod.c,v 1.16 2004/08/08 03:51:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define _(x) gettext((x))

#ifndef TEST_VERSION

#if defined(WIN32) || defined(__CYGWIN__)


#include "miscadmin.h"
#include <winioctl.h>

#undef rename
#undef unlink

/*
 *	pgrename
 */
int
pgrename(const char *from, const char *to)
{
	int			loops = 0;

#ifdef WIN32
	while (!MoveFileEx(from, to, MOVEFILE_REPLACE_EXISTING))
#endif
#ifdef __CYGWIN__
	while (rename(from, to) < 0)
#endif
	{
#ifdef WIN32
		if (GetLastError() != ERROR_ACCESS_DENIED)
#endif
#ifdef __CYGWIN__
		if (errno != EACCES)
#endif
			/* set errno? */
			return -1;
		pg_usleep(100000);				/* us */
		if (loops == 30)
#ifndef FRONTEND
			elog(LOG, "could not rename \"%s\" to \"%s\", continuing to try",
				 from, to);
#else
			fprintf(stderr, "could not rename \"%s\" to \"%s\", continuing to try\n",
					from, to);
#endif
		loops++;
	}

	if (loops > 30)
#ifndef FRONTEND
		elog(LOG, "completed rename of \"%s\" to \"%s\"", from, to);
#else
		fprintf(stderr, "completed rename of \"%s\" to \"%s\"\n", from, to);
#endif
	return 0;
}


/*
 *	pgunlink
 */
int
pgunlink(const char *path)
{
	int			loops = 0;

	while (unlink(path))
	{
		if (errno != EACCES)
			/* set errno? */
			return -1;
		pg_usleep(100000);		/* us */
		if (loops == 30)
#ifndef FRONTEND
			elog(LOG, "could not unlink \"%s\", continuing to try",
				 path);
#else
			fprintf(stderr, "could not unlink \"%s\", continuing to try\n",
					path);
#endif
		loops++;
	}

	if (loops > 30)
#ifndef FRONTEND
		elog(LOG, "completed unlink of \"%s\"", path);
#else
		fprintf(stderr, "completed unlink of \"%s\"\n", path);
#endif
	return 0;
}


/*
 *	pgsymlink support:
 *
 *	This struct is a replacement for REPARSE_DATA_BUFFER which is defined in VC6 winnt.h
 *	but omitted in later SDK functions.
 *	We only need the SymbolicLinkReparseBuffer part of the original struct's union.
 */
typedef struct
{
    DWORD  ReparseTag;
    WORD   ReparseDataLength;
    WORD   Reserved;
    /* SymbolicLinkReparseBuffer */
        WORD   SubstituteNameOffset;
        WORD   SubstituteNameLength;
        WORD   PrintNameOffset;
        WORD   PrintNameLength;
        WCHAR PathBuffer[1];
}
REPARSE_JUNCTION_DATA_BUFFER;

#define REPARSE_JUNCTION_DATA_BUFFER_HEADER_SIZE   \
		FIELD_OFFSET(REPARSE_JUNCTION_DATA_BUFFER, SubstituteNameOffset)


/*
 *	pgsymlink - uses Win32 junction points
 *
 *	For reference:	http://www.codeproject.com/w2k/junctionpoints.asp
 */
int
pgsymlink(const char *oldpath, const char *newpath)
{
	HANDLE dirhandle;
	DWORD len;
	char buffer[MAX_PATH*sizeof(WCHAR) + sizeof(REPARSE_JUNCTION_DATA_BUFFER)];
	char nativeTarget[MAX_PATH];
	char *p = nativeTarget;
	REPARSE_JUNCTION_DATA_BUFFER *reparseBuf = (REPARSE_JUNCTION_DATA_BUFFER*)buffer;
    
	CreateDirectory(newpath, 0);
	dirhandle = CreateFile(newpath, GENERIC_READ | GENERIC_WRITE, 
			0, 0, OPEN_EXISTING, 
			FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, 0);
    
	if (dirhandle == INVALID_HANDLE_VALUE)
		return -1;
    
	/* make sure we have an unparsed native win32 path */
	if (memcmp("\\??\\", oldpath, 4))
		sprintf(nativeTarget, "\\??\\%s", oldpath);
	else
		strcpy(nativeTarget, oldpath);
    
	while ((p = strchr(p, '/')) != 0)
		*p++ = '\\';

	len = strlen(nativeTarget) * sizeof(WCHAR);
	reparseBuf->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	reparseBuf->ReparseDataLength = len + 12;
	reparseBuf->Reserved = 0;
	reparseBuf->SubstituteNameOffset = 0;
	reparseBuf->SubstituteNameLength = len;
	reparseBuf->PrintNameOffset = len+sizeof(WCHAR);
	reparseBuf->PrintNameLength = 0;
	MultiByteToWideChar(CP_ACP, 0, nativeTarget, -1,
						reparseBuf->PathBuffer, MAX_PATH);
    
	/*
	 * FSCTL_SET_REPARSE_POINT is coded differently depending on SDK version;
	 * we use our own definition
	 */
	if (!DeviceIoControl(dirhandle, 
		CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 41, METHOD_BUFFERED, FILE_ANY_ACCESS),
		reparseBuf, 
		reparseBuf->ReparseDataLength + REPARSE_JUNCTION_DATA_BUFFER_HEADER_SIZE,
		0, 0, &len, 0))
	{
		LPSTR msg;

		errno=0;
		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
					  NULL, GetLastError(), 
					  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
					  (LPSTR)&msg, 0, NULL );
#ifdef FRONTEND
		fprintf(stderr, "Error setting junction for %s: %s", nativeTarget, msg);
#else
		ereport(ERROR, (errcode_for_file_access(),
			errmsg("Error setting junction for %s: %s", nativeTarget, msg)));
#endif
		LocalFree(msg);
	    
		CloseHandle(dirhandle);
		RemoveDirectory(newpath);
		return -1;
	}

	CloseHandle(dirhandle);

	return 0;
}

#endif


/* We undefined this above, so we redefine it */
#if defined(WIN32) || defined(__CYGWIN__)
#define unlink(path)	pgunlink(path)
#endif

/*
 *	rmt_cleanup
 *
 *	deallocate memory used for filenames
 */
static void
rmt_cleanup(char ** filenames)
{
	char ** fn;

	for (fn = filenames; *fn; fn++)
#ifdef FRONTEND
		free(*fn);

	free(filenames);
#else
		pfree(*fn);

	pfree(filenames);
#endif
}


/*
 *	rmtree
 *
 *	Delete a directory tree recursively.
 *	Assumes path points to a valid directory.
 *	Deletes everything under path.
 *	If rmtopdir is true deletes the directory too.
 *
 */
bool
rmtree(char *path, bool rmtopdir)
{
	char		filepath[MAXPGPATH];
	DIR		   *dir;
	struct dirent *file;
	char	  **filenames;
	char	  **filename;
	int			numnames = 0;
	struct stat statbuf;

	/*
	 * we copy all the names out of the directory before we start
	 * modifying it.
	 */

	dir = opendir(path);
	if (dir == NULL)
		return false;

	while ((file = readdir(dir)) != NULL)
	{
		if (strcmp(file->d_name, ".") != 0 && strcmp(file->d_name, "..") != 0)
			numnames++;
	}

	rewinddir(dir);

#ifdef FRONTEND
	if ((filenames = malloc((numnames + 2) * sizeof(char *))) == NULL)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(1);
	}
#else
	filenames = palloc((numnames + 2) * sizeof(char *));
#endif

	numnames = 0;

	while ((file = readdir(dir)) != NULL)
	{
		if (strcmp(file->d_name, ".") != 0 && strcmp(file->d_name, "..") != 0)
#ifdef FRONTEND
			if ((filenames[numnames++] = strdup(file->d_name)) == NULL)
		    {
				fprintf(stderr, _("out of memory\n"));
				exit(1);
			}
#else
			filenames[numnames++] = pstrdup(file->d_name);
#endif
	}

	filenames[numnames] = NULL;

	closedir(dir);

	/* now we have the names we can start removing things */

	for (filename = filenames; *filename; filename++)
	{
		snprintf(filepath, MAXPGPATH, "%s/%s", path, *filename);

		if (stat(filepath, &statbuf) != 0)
		{
			rmt_cleanup(filenames);
			return false;
		}

		if (S_ISDIR(statbuf.st_mode))
		{
			/* call ourselves recursively for a directory */
			if (!rmtree(filepath, true))
			{
				rmt_cleanup(filenames);
				return false;
			}
		}
		else
		{
			if (unlink(filepath) != 0)
			{
				rmt_cleanup(filenames);
				return false;
			}
		}
	}

	if (rmtopdir)
	{
		if (rmdir(path) != 0)
		{
			rmt_cleanup(filenames);
			return false;
		}
	}

	rmt_cleanup(filenames);
	return true;
}


#else


/*
 *	Illustrates problem with Win32 rename() and unlink()
 *	under concurrent access.
 *
 *	Run with arg '1', then less than 5 seconds later, run with
 *	 arg '2' (rename) or '3'(unlink) to see the problem.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <windows.h>

#define halt(str) \
do { \
	fputs(str, stderr); \
	exit(1); \
} while (0)

int
main(int argc, char *argv[])
{
	FILE	   *fd;

	if (argc != 2)
		halt("Arg must be '1' (test), '2' (rename), or '3' (unlink)\n"
			 "Run '1' first, then less than 5 seconds later, run\n"
			 "'2' to test rename, or '3' to test unlink.\n");

	if (atoi(argv[1]) == 1)
	{
		if ((fd = fopen("/rtest.txt", "w")) == NULL)
			halt("Can not create file\n");
		fclose(fd);
		if ((fd = fopen("/rtest.txt", "r")) == NULL)
			halt("Can not open file\n");
		Sleep(5000);
	}
	else if (atoi(argv[1]) == 2)
	{
		unlink("/rtest.new");
		if ((fd = fopen("/rtest.new", "w")) == NULL)
			halt("Can not create file\n");
		fclose(fd);
		while (!MoveFileEx("/rtest.new", "/rtest.txt", MOVEFILE_REPLACE_EXISTING))
		{
			if (GetLastError() != ERROR_ACCESS_DENIED)
				halt("Unknown failure\n");
			else
				fprintf(stderr, "move failed\n");
			Sleep(500);
		}
		halt("move successful\n");
	}
	else if (atoi(argv[1]) == 3)
	{
		while (unlink("/rtest.txt"))
		{
			if (errno != EACCES)
				halt("Unknown failure\n");
			else
				fprintf(stderr, "unlink failed\n");
			Sleep(500);
		}
		halt("unlink successful\n");
	}
	else
		halt("invalid arg\n");

	return 0;
}

#endif
