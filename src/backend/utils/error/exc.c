/*-------------------------------------------------------------------------
 *
 * exc.c--
 *    POSTGRES exception handling code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/error/Attic/exc.c,v 1.15 1997/08/19 21:35:17 momjian Exp $
 *
 * NOTE
 *    XXX this code needs improvement--check for state violations and
 *    XXX reset after handling an exception.
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>	/* XXX use own I/O routines */
#include <errno.h>
#include <string.h>

#include "postgres.h"

#include "utils/exc.h"
#include "storage/ipc.h"

static void ExcUnCaught(Exception *excP, ExcDetail detail, ExcData data,
			ExcMessage message);
static void ExcPrint(Exception *excP, ExcDetail detail, ExcData data,
		     ExcMessage message);

/*
 * Global Variables
 */
static bool ExceptionHandlingEnabled = false;

char*			ExcFileName = NULL;
Index			ExcLineNumber = 0;

ExcFrame		*ExcCurFrameP = NULL;

static	ExcProc		*ExcUnCaughtP = NULL;

extern	char*	ProgramName;

/*
 * Exported Functions
 */

/*
 * EnableExceptionHandling --
 *	Enables/disables the exception handling system.
 *
 * Note:
 *	This must be called before any exceptions occur.  I.e., call this first!
 *	This routine will not return if an error is detected.
 *	This does not follow the usual Enable... protocol.
 *	This should be merged more closely with the error logging and tracing
 *	packages.
 *
 * Exceptions:
 *	none
 */
/*
 * Excection handling should be supported by the language, thus there should
 * be no need to explicitly enable exception processing.
 *
 * This function should probably not be called, ever.  Currently it does
 * almost nothing.  If there is a need for this intialization and checking.
 * then this function should be converted to the new-style Enable code and
 * called by all the other module Enable functions.
 */
void
EnableExceptionHandling(bool on)
{
    if (on == ExceptionHandlingEnabled) {
	/* XXX add logging of failed state */
	exitpg(255);
	/* ExitPostgres(FatalExitStatus); */
    }
    
    if (on) {	/* initialize */
	;
    } else {	/* cleanup */
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
    extern	int	errno;
    extern	int	sys_nerr;
    
#ifdef	lint
    data = data;
#endif
    
    fflush(stdout);	/* In case stderr is buffered */
    
#if	0
    if (ProgramName != NULL && *ProgramName != '\0')
	fprintf(stderr, "%s: ", ProgramName);
#endif
    
    if (message != NULL)
	fprintf(stderr, "%s", message);
    else if (excP->message != NULL)
	fprintf(stderr, "%s", excP->message);
    else
#ifdef	lint
	fprintf(stderr, "UNNAMED EXCEPTION 0x%lx", excP);
#else
    fprintf(stderr, "UNNAMED EXCEPTION 0x%lx", (long)excP);
#endif
    
    fprintf(stderr, " (%ld)", detail);
    
    if (errno > 0 && errno < sys_nerr) 
	fprintf(stderr, " [%s]", strerror(errno));
    else if (errno != 0)
	fprintf(stderr, " [Error %d]", errno);
    
    fprintf(stderr, "\n");
    
    fflush(stderr);
}

#ifdef NOT_USED
ExcProc *
ExcGetUnCaught(void)
{
    return (ExcUnCaughtP);
}
#endif

#ifdef NOT_USED
ExcProc *
ExcSetUnCaught(ExcProc *newP)
{
    ExcProc	*oldP = ExcUnCaughtP;
    
    ExcUnCaughtP = newP;
    
    return (oldP);
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
	 ExcData	data,
	 ExcMessage message)
{
    register ExcFrame	*efp;
    
    efp = ExcCurFrameP;
    if (efp == NULL) {
	if (ExcUnCaughtP != NULL)
	    (*ExcUnCaughtP)(excP, detail, data, message);
	
	ExcUnCaught(excP, detail, data, message);
    } else {
	efp->id		= excP;
	efp->detail	= detail;
	efp->data	= data;
	efp->message	= message;
	
	ExcCurFrameP = efp->link;
	
#if defined (JMP_BUF)
	longjmp(efp->context, 1);
#else
	siglongjmp(efp->context, 1);
#endif
    }
}
