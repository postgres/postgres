/*
 *	While "xcopy /e /i /q" works fine for copying directories, on Windows XP
 *	it requires an Window handle which prevents it from working when invoked
 *	as a service.
 */

#include "postgres.h"

int
copydir(char *fromdir,char *todir)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		fromfl[MAXPGPATH];
	char		tofl[MAXPGPATH];

	if (mkdir(todir) != 0)
	{
		elog(ERROR, "could not make directory '%s'",todir);
		return 1;
	}
	xldir = opendir(fromdir);
	if (xldir == NULL)
	{
		closedir(xldir);
		elog(ERROR, "could not open directory '%s'",fromdir);
		return 1;
	}

	while ((xlde = readdir(xldir)) != NULL)
	{
			snprintf(fromfl, MAXPGPATH, "%s/%s", fromdir, xlde->d_name);
			snprintf(tofl, MAXPGPATH, "%s/%s", todir, xlde->d_name);
			if (CopyFile(fromfl,tofl,TRUE) < 0)
			{
				closedir(xldir);
				elog(ERROR,"could not create file %s\n",todir);
				return 1;
			}
	}

	closedir(xldir);
	return 0;
}
