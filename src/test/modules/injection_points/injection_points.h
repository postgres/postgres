/*-------------------------------------------------------------------------
 *
 * injection_points.h
 *		Definitions for the injection points module
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/test/modules/injection_points/injection_points.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef INJECTION_POINTS_H
#define INJECTION_POINTS_H

/* Maximum length for an injection point condition string */
#define INJ_DATA_MAXLEN 256

typedef enum InjectionPointConditionType
{
	INJ_CONDITION_NONE = 0,		/* no restrictions to apply */
	INJ_CONDITION_PID = 1 << 0, /* PID restriction */
	INJ_CONDITION_STRING = 1 << 1,	/* string to compare with arg */
} InjectionPointConditionType;

typedef struct InjectionPointCondition
{
	/* Type of the condition */
	InjectionPointConditionType type;

	/* ID of the process where the injection point is allowed to run */
	int			pid;

	/* String to compare with an argument at runtime */
	char		str[INJ_DATA_MAXLEN];
} InjectionPointCondition;

#endif							/* INJECTION_POINTS_H */
