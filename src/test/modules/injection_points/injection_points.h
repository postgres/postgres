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

typedef enum InjectionPointConditionType
{
	INJ_CONDITION_ALWAYS = 0,	/* always run */
	INJ_CONDITION_PID,			/* PID restriction */
} InjectionPointConditionType;

typedef struct InjectionPointCondition
{
	/* Type of the condition */
	InjectionPointConditionType type;

	/* ID of the process where the injection point is allowed to run */
	int			pid;
} InjectionPointCondition;

#endif							/* INJECTION_POINTS_H */
