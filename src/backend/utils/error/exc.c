/*-------------------------------------------------------------------------
 *
 * exc.c
 *	  POSTGRES exception handling code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/error/Attic/exc.c,v 1.36 2001/01/24 19:43:15 momjian Exp $
 *
 * NOTE
 *	  XXX this code needs improvement--check for state violations and
 *	  XXX reset after handling an exception.
 *	  XXX Probably should be merged with elog.c.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>

#include "storage/ipc.h"
#include "utils/exc.h"

extern int	errno;


static void ExcUnCaught(Exception *excP, ExcDetail detail, ExcData data,
			ExcMessage message);
static void ExcPrint(Exception *excP, ExcDetail detail, ExcData data,
		 ExcMessage message);

/*
 * Global Variables
 */
static bool ExceptionHandlingEnabled = false;

char	   *ExcFileName = NULL;
Index		ExcLineNumber = 0;

ExcFrame   *ExcCurFrameP = NULL;

static ExcProc *ExcUnCaughtP = NULL;

/*
 * Exported Functions
 */

/*
 * EnableExceptionHandling
 *		Enables/disables the exception handling system.
 *
 * Note:
 *		This must be called before any exceptions occur.  I.e., call this first!
 *		This routine will not return if an error is detected.
 *		This does not follow the usual Enable... protocol.
 *		This should be merged more closely with the error logging and tracing
 *		packages.
 *
 * Exceptions:
 *		none
 */
/*
 * Excection handling should be supported by the language, thus there should
 * be no need to explicitly enable exception processing.
 *
 * This function should probably not be called, ever.  Currently it does
 * almost nothing.	If there is a need for this intialization and checking.
 * then this function should be converted to the new-style Enable code and
 * called by all the other module Enable functions.
 */
void
EnableExceptionHandling(bool on)
{
	if (on == ExceptionHandlingEnabled)
	{
		/* XXX add logging of failed state */
		proc_exit(255);
		/* ExitPostgres(FatalExitStatus); */
	}

	if (on)
	{							/* initialize */
		;
	}
	else
	{							/* cleanup */
		ExcFileName = NULL;
		ExcLineNumber = 0;
		ExcCurFrameP = NULL;
		ExcUnCaughtP = NULL;
	}

	ExceptionHandlingEnabled = on;
}

static void
ExcPrint(Exception *excP,
		 ExcDetail detail,
		 ExcData data,
		 ExcMessage message)
{
	/* this buffer is only used if errno has a bogus value: */
	char		errorstr_buf[32];
	const char *errorstr;

#ifdef	lint
	data = data;
#endif

	/* Save error str before calling any function that might change errno */
	errorstr = strerror(errno);
	/*
	 * Some strerror()s return an empty string for out-of-range errno.
	 * This is ANSI C spec compliant, but not exactly useful.
	 */
	if (errorstr == NULL || *errorstr == '\0')
	{
		sprintf(errorstr_buf, "error %d", errno);
		errorstr = errorstr_buf;
	}

	fflush(stdout);				/* In case stderr is buffered */

	if (message != NULL)
		fprintf(stderr, "%s", message);
	else if (excP->message != NULL)
		fprintf(stderr, "%s", excP->message);
	else
		fprintf(stderr, "UNNAMED EXCEPTION %p", excP);

	fprintf(stderr, " (%ld) [%s]\n", detail, errorstr);

	fflush(stderr);
}

#ifdef NOT_USED
ExcProc    *
ExcGetUnCaught(void)
{
	return ExcUnCaughtP;
}

#endif

#ifdef NOT_USED
ExcProc    *
ExcSetUnCaught(ExcProc *newP)
{
	ExcProc    *oldP = ExcUnCaughtP;

	ExcUnCaughtP = newP;

	return oldP;
}

#endif

static void
ExcUnCaught(Exception *excP,
			ExcDetail detail,
			ExcData data,
			ExcMessage message)
{
	ExcPrint(excP, detail, data, message);

	ExcAbort(excP, detail, data, message);
}

void
ExcRaise(Exception *excP,
		 ExcDetail detail,
		 ExcData data,
		 ExcMessage message)
{
	ExcFrame   *efp;

	efp = ExcCurFrameP;
	if (efp == NULL)
	{
		if (ExcUnCaughtP != NULL)
			(*ExcUnCaughtP) (excP, detail, data, message);

		ExcUnCaught(excP, detail, data, message);
	}
	else
	{
		efp->id = excP;
		efp->detail = detail;
		efp->data = data;
		efp->message = message;

		ExcCurFrameP = efp->link;

		siglongjmp(efp->context, 1);
	}
}
