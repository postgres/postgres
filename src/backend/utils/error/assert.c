/*-------------------------------------------------------------------------
 *
 * assert.c--
 *    Assert code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/error/assert.c,v 1.1.1.1 1996/07/09 06:22:07 scrappy Exp $
 *
 * NOTE
 *    This should eventually work with elog(), dlog(), etc.
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>

#include "c.h"			/* where the declaration goes */
#include "utils/module.h"

#include "utils/exc.h"

int
ExceptionalCondition(char* conditionName,
		     Exception *exceptionP,
		     char* detail,
		     char* fileName,
		     int lineNumber)
{
    extern char* ExcFileName;	/* XXX */
    extern Index ExcLineNumber;	/* XXX */
    
    ExcFileName = fileName;
    ExcLineNumber = lineNumber;
    
    if (!PointerIsValid(conditionName)
	|| !PointerIsValid(fileName)
	|| !PointerIsValid(exceptionP)) {
	fprintf(stderr, "ExceptionalCondition: bad arguments\n");
	
	ExcAbort(exceptionP, 
		 (ExcDetail)detail,
		 (ExcData)NULL,
		 (ExcMessage)NULL);
    } else {
	fprintf(stderr,
		"%s(\"%s:%s\", File: \"%s\", Line: %d)\n",
		exceptionP->message, conditionName, detail,
		fileName, lineNumber);
    }
    
    /*
     * XXX Depending on the Exception and tracing conditions, you will
     * XXX want to stop here immediately and maybe dump core.
     * XXX This may be especially true for Assert(), etc.
     */
    
    /* TraceDump();	dump the trace stack */
    
    /* XXX FIXME: detail is lost */
    ExcRaise(exceptionP, (ExcDetail)0, (ExcData)NULL, conditionName);
    return(0);
}
