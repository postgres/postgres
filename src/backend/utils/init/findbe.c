/*-------------------------------------------------------------------------
 *
 * findbe.c
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/Attic/findbe.c,v 1.37 2003/08/04 02:40:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "miscadmin.h"

#ifndef S_IRUSR					/* XXX [TRH] should be in a header */
#define S_IRUSR		 S_IREAD
#define S_IWUSR		 S_IWRITE
#define S_IXUSR		 S_IEXEC
#define S_IRGRP		 ((S_IRUSR)>>3)
#define S_IWGRP		 ((S_IWUSR)>>3)
#define S_IXGRP		 ((S_IXUSR)>>3)
#define S_IROTH		 ((S_IRUSR)>>6)
#define S_IWOTH		 ((S_IWUSR)>>6)
#define S_IXOTH		 ((S_IXUSR)>>6)
#endif


/*
 * ValidateBinary -- validate "path" as a POSTMASTER/POSTGRES executable file
 *
 * returns 0 if the file is found and no error is encountered.
 *		  -1 if the regular file "path" does not exist or cannot be executed.
 *		  -2 if the file is otherwise valid but cannot be read.
 */
static int
ValidateBinary(char *path)
{
	struct stat buf;

#ifndef WIN32
	uid_t		euid;
	struct group *gp;
	struct passwd *pwp;
#endif
	int			i;
	int			is_r = 0;
	int			is_x = 0;
	int			in_grp = 0;

	/*
	 * Ensure that the file exists and is a regular file.
	 *
	 * XXX if you have a broken system where stat() looks at the symlink
	 * instead of the underlying file, you lose.
	 */
	if (stat(path, &buf) < 0)
	{
		elog(DEBUG3, "could not stat \"%s\": %m", path);
		return -1;
	}

	if ((buf.st_mode & S_IFMT) != S_IFREG)
	{
		elog(DEBUG3, "\"%s\" is not a regular file", path);
		return -1;
	}

	/*
	 * Ensure that we are using an authorized backend.
	 *
	 * XXX I'm open to suggestions here.  I would like to enforce ownership
	 * of binaries by user "postgres" but people seem to like to run as
	 * users other than "postgres"...
	 */

	/*
	 * Ensure that the file is both executable and readable (required for
	 * dynamic loading).
	 */
#ifdef WIN32
	is_r = buf.st_mode & S_IRUSR;
	is_x = buf.st_mode & S_IXUSR;
	return is_x ? (is_r ? 0 : -2) : -1;
#else
	euid = geteuid();
	if (euid == buf.st_uid)
	{
		is_r = buf.st_mode & S_IRUSR;
		is_x = buf.st_mode & S_IXUSR;
		if (!(is_r && is_x))
			elog(DEBUG3, "\"%s\" is not user read/execute", path);
		return is_x ? (is_r ? 0 : -2) : -1;
	}
	pwp = getpwuid(euid);
	if (pwp)
	{
		if (pwp->pw_gid == buf.st_gid)
			++in_grp;
		else if (pwp->pw_name &&
				 (gp = getgrgid(buf.st_gid)) != NULL &&
				 gp->gr_mem != NULL)
		{
			for (i = 0; gp->gr_mem[i]; ++i)
			{
				if (!strcmp(gp->gr_mem[i], pwp->pw_name))
				{
					++in_grp;
					break;
				}
			}
		}
		if (in_grp)
		{
			is_r = buf.st_mode & S_IRGRP;
			is_x = buf.st_mode & S_IXGRP;
			if (!(is_r && is_x))
				elog(DEBUG3, "\"%s\" is not group read/execute", path);
			return is_x ? (is_r ? 0 : -2) : -1;
		}
	}
	is_r = buf.st_mode & S_IROTH;
	is_x = buf.st_mode & S_IXOTH;
	if (!(is_r && is_x))
		elog(DEBUG3, "\"%s\" is not other read/execute", path);
	return is_x ? (is_r ? 0 : -2) : -1;
#endif
}

/*
 * FindExec -- find an absolute path to a valid backend executable
 *
 * The reason we have to work so hard to find an absolute path is that
 * on some platforms we can't do dynamic loading unless we know the
 * executable's location.  Also, we need a full path not a relative
 * path because we will later change working directory.
 */
int
FindExec(char *full_path, const char *argv0, const char *binary_name)
{
	char		buf[MAXPGPATH + 2];
	char	   *p;
	char	   *path,
			   *startp,
			   *endp;

	/*
	 * for the postmaster: First try: use the binary that's located in the
	 * same directory as the postmaster, if it was invoked with an
	 * explicit path. Presumably the user used an explicit path because it
	 * wasn't in PATH, and we don't want to use incompatible executables.
	 *
	 * This has the neat property that it works for installed binaries, old
	 * source trees (obj/support/post{master,gres}) and new marc source
	 * trees (obj/post{master,gres}) because they all put the two binaries
	 * in the same place.
	 *
	 * for the binary: First try: if we're given some kind of path, use it
	 * (making sure that a relative path is made absolute before returning
	 * it).
	 */
	if (argv0 && (p = last_path_separator(argv0)) && *++p)
	{
		if (is_absolute_path(argv0) || !getcwd(buf, MAXPGPATH))
			buf[0] = '\0';
		else
			strcat(buf, "/");
		strcat(buf, argv0);
		p = last_path_separator(buf);
		strcpy(++p, binary_name);
		if (ValidateBinary(buf) == 0)
		{
			strncpy(full_path, buf, MAXPGPATH);
			elog(DEBUG2, "found \"%s\" using argv[0]", full_path);
			return 0;
		}
		elog(DEBUG2, "invalid binary \"%s\"", buf);
		return -1;
	}

	/*
	 * Second try: since no explicit path was supplied, the user must have
	 * been relying on PATH.  We'll use the same PATH.
	 */
	if ((p = getenv("PATH")) && *p)
	{
		elog(DEBUG2, "searching PATH for executable");
		path = strdup(p);		/* make a modifiable copy */
		for (startp = path, endp = strchr(path, ':');
			 startp && *startp;
			 startp = endp + 1, endp = strchr(startp, ':'))
		{
			if (startp == endp) /* it's a "::" */
				continue;
			if (endp)
				*endp = '\0';
			if (is_absolute_path(startp) || !getcwd(buf, MAXPGPATH))
				buf[0] = '\0';
			else
				strcat(buf, "/");
			strcat(buf, startp);
			strcat(buf, "/");
			strcat(buf, binary_name);
			switch (ValidateBinary(buf))
			{
				case 0: /* found ok */
					strncpy(full_path, buf, MAXPGPATH);
					elog(DEBUG2, "found \"%s\" using PATH", full_path);
					free(path);
					return 0;
				case -1:		/* wasn't even a candidate, keep looking */
					break;
				case -2:		/* found but disqualified */
					elog(DEBUG2, "could not read binary \"%s\"", buf);
					free(path);
					return -1;
			}
			if (!endp)			/* last one */
				break;
		}
		free(path);
	}

	elog(DEBUG2, "could not find a \"%s\" to execute", binary_name);
	return -1;
}
