/*-------------------------------------------------------------------------
 *
 * dirmod.c
 *	  rename/unlink()
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	These are replacement versions of unlink and rename that work on
 *	Win32 (NT, Win2k, XP).	replace() doesn't work on Win95/98/Me.
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/dirmod.c,v 1.36 2005/02/22 04:43:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

/* Don't modify declarations in system headers */
#if defined(WIN32) || defined(__CYGWIN__)
#undef rename
#undef unlink
#endif

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#if defined(WIN32) || defined(__CYGWIN__)
#ifndef __CYGWIN__
#include <winioctl.h>
#else
#include <windows.h>
#include <w32api/winioctl.h>
#endif
#endif


#ifndef FRONTEND

/*
 *	On Windows, call non-macro versions of palloc; we can't reference
 *	CurrentMemoryContext in this file because of DLLIMPORT conflict.
 */
#if defined(WIN32) || defined(__CYGWIN__)
#undef palloc
#undef pstrdup
#define palloc(sz)		pgport_palloc(sz)
#define pstrdup(str)	pgport_pstrdup(str)
#endif

#else /* FRONTEND */

/*
 *	In frontend, fake palloc behavior with these
 */
#undef palloc
#undef pstrdup
#define palloc(sz)		fe_palloc(sz)
#define pstrdup(str)	fe_pstrdup(str)
#define repalloc(pointer,sz)	fe_repalloc(pointer,sz)
#define pfree(pointer)	free(pointer)

static void *
fe_palloc(Size size)
{
	void	   *res;

	if ((res = malloc(size)) == NULL)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(1);
	}
	return res;
}

static char *
fe_pstrdup(const char *string)
{
	char	   *res;

	if ((res = strdup(string)) == NULL)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(1);
	}
	return res;
}

static void *
fe_repalloc(void *pointer, Size size)
{
	void	   *res;

	if ((res = realloc(pointer, size)) == NULL)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(1);
	}
	return res;
}

#endif /* FRONTEND */


#if defined(WIN32) || defined(__CYGWIN__)

/*
 *	pgrename
 */
int
pgrename(const char *from, const char *to)
{
	int			loops = 0;

	/* Is this loop even necessary now that we have win32_open()?  */
#if defined(WIN32) && !defined(__CYGWIN__)
	while (!MoveFileEx(from, to, MOVEFILE_REPLACE_EXISTING))
#endif
#ifdef __CYGWIN__
		while (rename(from, to) < 0)
#endif
		{
#if defined(WIN32) && !defined(__CYGWIN__)
			if (GetLastError() != ERROR_ACCESS_DENIED)
#endif
#ifdef __CYGWIN__
				if (errno != EACCES)
#endif
					/* set errno? */
					return -1;
			pg_usleep(100000);	/* us */
			if (loops == 30)
#ifndef FRONTEND
				elog(LOG, "could not rename \"%s\" to \"%s\", continuing to try",
					 from, to);
#else
				fprintf(stderr, _("could not rename \"%s\" to \"%s\", continuing to try\n"),
						from, to);
#endif
			loops++;
		}

	if (loops > 30)
#ifndef FRONTEND
		elog(LOG, "completed rename of \"%s\" to \"%s\"", from, to);
#else
		fprintf(stderr, _("completed rename of \"%s\" to \"%s\"\n"), from, to);
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

	/* Is this loop even necessary now that we have win32_open()?  */
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
			fprintf(stderr, _("could not unlink \"%s\", continuing to try\n"),
					path);
#endif
		loops++;
	}

	if (loops > 30)
#ifndef FRONTEND
		elog(LOG, "completed unlink of \"%s\"", path);
#else
		fprintf(stderr, _("completed unlink of \"%s\"\n"), path);
#endif
	return 0;
}


#ifdef WIN32	/* Cygwin has its own symlinks */

/*
 *	pgsymlink support:
 *
 *	This struct is a replacement for REPARSE_DATA_BUFFER which is defined in VC6 winnt.h
 *	but omitted in later SDK functions.
 *	We only need the SymbolicLinkReparseBuffer part of the original struct's union.
 */
typedef struct
{
	DWORD		ReparseTag;
	WORD		ReparseDataLength;
	WORD		Reserved;
	/* SymbolicLinkReparseBuffer */
	WORD		SubstituteNameOffset;
	WORD		SubstituteNameLength;
	WORD		PrintNameOffset;
	WORD		PrintNameLength;
	WCHAR		PathBuffer[1];
}	REPARSE_JUNCTION_DATA_BUFFER;

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
	HANDLE		dirhandle;
	DWORD		len;
	char		buffer[MAX_PATH * sizeof(WCHAR) + sizeof(REPARSE_JUNCTION_DATA_BUFFER)];
	char		nativeTarget[MAX_PATH];
	char	   *p = nativeTarget;
	REPARSE_JUNCTION_DATA_BUFFER *reparseBuf = (REPARSE_JUNCTION_DATA_BUFFER *) buffer;

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
	reparseBuf->PrintNameOffset = len + sizeof(WCHAR);
	reparseBuf->PrintNameLength = 0;
	MultiByteToWideChar(CP_ACP, 0, nativeTarget, -1,
						reparseBuf->PathBuffer, MAX_PATH);

	/*
	 * FSCTL_SET_REPARSE_POINT is coded differently depending on SDK
	 * version; we use our own definition
	 */
	if (!DeviceIoControl(dirhandle,
						 CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 41, METHOD_BUFFERED, FILE_ANY_ACCESS),
						 reparseBuf,
						 reparseBuf->ReparseDataLength + REPARSE_JUNCTION_DATA_BUFFER_HEADER_SIZE,
						 0, 0, &len, 0))
	{
		LPSTR		msg;

		errno = 0;
		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
					  NULL, GetLastError(),
					  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
					  (LPSTR) & msg, 0, NULL);
#ifndef FRONTEND
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("Error setting junction for %s: %s",
						nativeTarget, msg)));
#else
		fprintf(stderr, _("Error setting junction for %s: %s\n"),
				nativeTarget, msg);
#endif
		LocalFree(msg);

		CloseHandle(dirhandle);
		RemoveDirectory(newpath);
		return -1;
	}

	CloseHandle(dirhandle);

	return 0;
}

#endif /* WIN32 */

#endif /* defined(WIN32) || defined(__CYGWIN__) */


/* We undefined this above, so we redefine it */
#if defined(WIN32) || defined(__CYGWIN__)
#define unlink(path)	pgunlink(path)
#endif


/*
 * fnames
 *
 * return a list of the names of objects in the argument directory 
 */
static char **
fnames(char *path)
{
	DIR		   *dir;
	struct dirent *file;
	char	  **filenames;
	int			numnames = 0;
	int			fnsize = 200;	/* enough for many small dbs */

	dir = opendir(path);
	if (dir == NULL)
		return NULL;

	filenames = (char **) palloc(fnsize * sizeof(char *));

	while ((file = readdir(dir)) != NULL)
	{
		if (strcmp(file->d_name, ".") != 0 && strcmp(file->d_name, "..") != 0)
		{
			if (numnames+1 >= fnsize)
			{
				fnsize *= 2;
				filenames = (char **) repalloc(filenames,
											   fnsize * sizeof(char *));
			}
			filenames[numnames++] = pstrdup(file->d_name);
		}
	}

	filenames[numnames] = NULL;

	closedir(dir);

	return filenames;
}


/*
 *	fnames_cleanup
 *
 *	deallocate memory used for filenames
 */
static void
fnames_cleanup(char **filenames)
{
	char	  **fn;

	for (fn = filenames; *fn; fn++)
		pfree(*fn);

	pfree(filenames);
}


/*
 *	rmtree
 *
 *	Delete a directory tree recursively.
 *	Assumes path points to a valid directory.
 *	Deletes everything under path.
 *	If rmtopdir is true deletes the directory too.
 */
bool
rmtree(char *path, bool rmtopdir)
{
	char		filepath[MAXPGPATH];
	char	  **filenames;
	char	  **filename;
	struct stat statbuf;

	/*
	 * we copy all the names out of the directory before we start
	 * modifying it.
	 */
	filenames = fnames(path);

	if (filenames == NULL)
		return false;

	/* now we have the names we can start removing things */

	for (filename = filenames; *filename; filename++)
	{
		snprintf(filepath, MAXPGPATH, "%s/%s", path, *filename);

		if (stat(filepath, &statbuf) != 0)
			goto report_and_fail;

		if (S_ISDIR(statbuf.st_mode))
		{
			/* call ourselves recursively for a directory */
			if (!rmtree(filepath, true))
			{
				/* we already reported the error */
				fnames_cleanup(filenames);
				return false;
			}
		}
		else
		{
			if (unlink(filepath) != 0)
				goto report_and_fail;
		}
	}

	if (rmtopdir)
	{
		if (rmdir(path) != 0)
			goto report_and_fail;
	}

	fnames_cleanup(filenames);
	return true;

report_and_fail:

#ifndef FRONTEND
	elog(WARNING, "could not remove file or directory \"%s\": %m", filepath);
#else
	fprintf(stderr, _("could not remove file or directory \"%s\": %s\n"), filepath, strerror(errno));
#endif
	fnames_cleanup(filenames);
	return false;
}
