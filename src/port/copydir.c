/*
 *	While "xcopy /e /i /q" works fine for copying directories, on Windows XP
 *	it requires an Window handle which prevents it from working when invoked
 *	as a service.
 */

#include "postgres.h"

#undef mkdir	/* no reason to use that macro because we ignore the 2nd arg */

#include <dirent.h>


int
copydir(char *fromdir,char *todir)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		fromfl[MAXPGPATH];
	char		tofl[MAXPGPATH];

	if (mkdir(todir) != 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", todir)));
		return 1;
	}
	xldir = opendir(fromdir);
	if (xldir == NULL)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", fromdir)));
		return 1;
	}

	while ((xlde = readdir(xldir)) != NULL)
	{
			snprintf(fromfl, MAXPGPATH, "%s/%s", fromdir, xlde->d_name);
			snprintf(tofl, MAXPGPATH, "%s/%s", todir, xlde->d_name);
			if (CopyFile(fromfl,tofl,TRUE) < 0)
			{
				int		save_errno = errno;

				closedir(xldir);
				errno = save_errno;
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not copy file \"%s\": %m", fromfl)));
				return 1;
			}
	}

	closedir(xldir);
	return 0;
}
