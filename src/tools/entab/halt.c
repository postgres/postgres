/*
**
**		halt.c
**
** src/tools/entab/halt.c
**
**		This is used to print out error messages and exit
*/

#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


/*-------------------------------------------------------------------------
**
**		halt - print error message, and call clean up routine or exit
**
**------------------------------------------------------------------------*/

/*VARARGS*/
void
halt(const char *format,...)
{
	va_list		arg_ptr;
	const char *pstr;
	void		(*sig_func) ();

	va_start(arg_ptr, format);
	if (strncmp(format, "PERROR", 6) != 0)
		vfprintf(stderr, format, arg_ptr);
	else
	{
		for (pstr = format + 6; *pstr == ' ' || *pstr == ':'; pstr++)
			;
		vfprintf(stderr, pstr, arg_ptr);
		perror("");
	}
	va_end(arg_ptr);
	fflush(stderr);

	/* call one clean up function if defined */
	if ((sig_func = signal(SIGTERM, SIG_DFL)) != SIG_DFL &&
		sig_func != SIG_IGN)
		(*sig_func) (0);
	else if ((sig_func = signal(SIGHUP, SIG_DFL)) != SIG_DFL &&
			 sig_func != SIG_IGN)
		(*sig_func) (0);
	else if ((sig_func = signal(SIGINT, SIG_DFL)) != SIG_DFL &&
			 sig_func != SIG_IGN)
		(*sig_func) (0);
	else if ((sig_func = signal(SIGQUIT, SIG_DFL)) != SIG_DFL &&
			 sig_func != SIG_IGN)
		(*sig_func) (0);
	exit(1);
}
