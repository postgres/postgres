/*-------------------------------------------------------------------------
 *
 * ultrix4.c
 *	  This dynamic loader uses Andrew Yu's libdl-1.0 package for Ultrix 4.x.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/dynloader/ultrix4.c,v 1.26 2008/01/01 19:45:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "dl.h"
#include "utils/dynamic_loader.h"

extern char my_exec_path[];

void *
pg_dlopen(char *filename)
{
	static int	dl_initialized = 0;
	void	   *handle;

	/*
	 * initializes the dynamic loader with the executable's pathname. (only
	 * needs to do this the first time pg_dlopen is called.)
	 */
	if (!dl_initialized)
	{
		if (!dl_init(my_exec_path))
			return NULL;

		/*
		 * if there are undefined symbols, we want dl to search from the
		 * following libraries also.
		 */
		dl_setLibraries("/usr/lib/libm_G0.a:/usr/lib/libc_G0.a");
		dl_initialized = 1;
	}

	/*
	 * open the file. We do the symbol resolution right away so that we will
	 * know if there are undefined symbols. (This is in fact the same
	 * semantics as "ld -A". ie. you cannot have undefined symbols.
	 */
	if ((handle = dl_open(filename, DL_NOW)) == NULL)
	{
		int			count;
		char	  **list = dl_undefinedSymbols(&count);

		/* list the undefined symbols, if any */
		if (count)
		{
			while (*list)
			{
				elog(WARNING, "\"%s\" is undefined", *list);
				list++;
			}
		}
	}

	return (void *) handle;
}
