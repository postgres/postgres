/*-------------------------------------------------------------------------
 *
 * pgpa_planner.h
 *	  planner integration for pg_plan_advice
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_planner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPA_PLANNER_H
#define PGPA_PLANNER_H

extern void pgpa_planner_install_hooks(void);

extern int	pgpa_planner_generate_advice;

#endif
