/*-------------------------------------------------------------------------
 *
 * assert.c
 *	  Assert code.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/error/assert.c,v 1.25 2003/11/29 19:52:01 pgsql Exp $
 *
 * NOTE
 *	  This should eventually work with elog()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

/*
 * ExceptionalCondition - Handles the failure of an Assert()
 */
int
ExceptionalCondition(char *conditionName,
					 char *errorType,
					 char *fileName,
					 int lineNumber)
{
	if (!PointerIsValid(conditionName)
		|| !PointerIsValid(fileName)
		|| !PointerIsValid(errorType))
		fprintf(stderr, "TRAP: ExceptionalCondition: bad arguments\n");
	else
	{
		fprintf(stderr, "TRAP: %s(\"%s\", File: \"%s\", Line: %d)\n",
				errorType, conditionName,
				fileName, lineNumber);
	}

#ifdef SLEEP_ON_ASSERT
	sleep(1000000);
#endif

	abort();

	return 0;
}
