/* $Id: path.c,v 1.2 2003/08/04 00:43:33 momjian Exp $ */

#include "c.h"
#include <ctype.h>

/*
 *	is_absolute_path
 */
bool
is_absolute_path(const char *filename)
{
	return filename[0] == '/'
#ifdef WIN32					/* WIN32 paths can either have forward or
								 * backward slashes */
		|| filename[0] == '\\'
		|| (isalpha(filename[0]) && filename[1] == ':'
			&& (filename[2] == '\\' || filename[2] == '/'))
#endif
		;
}



/*
 *	first_path_separator
 */
char *
first_path_separator(const char *filename)
{
#ifndef WIN32
	return strchr(filename, '/');
#else
	char	   *slash,
			   *bslash;

	/* How should we handle "C:file.c"? */
	slash = strchr(filename, '/');
	bslash = strchr(filename, '\\');
	if (slash == NULL)
		return bslash;
	else if (bslash == NULL)
		return slash;
	else
		return (slash < bslash) ? slash : bslash;
#endif
}


/*
 *	last_path_separator
 */
char *
last_path_separator(const char *filename)
{
#ifndef WIN32
	return strrchr(filename, '/');
#else
	char	   *slash,
			   *bslash;

	/* How should we handle "C:file.c"? */
	slash = strrchr(filename, '/');
	bslash = strrchr(filename, '\\');
	if (slash == NULL)
		return bslash;
	else if (bslash == NULL)
		return slash;
	else
		return (slash > bslash) ? slash : bslash;
#endif
}


/*
 * Extracts the actual name of the program as called.
 */
char *
get_progname(char *argv0)
{
	if (!last_path_separator(argv0))
		return argv0;
	else
		return last_path_separator(argv0) + 1;
}
