/*-------------------------------------------------------------------------
 *
 * pgpa_output.h
 *	  produce textual output from the results of a plan tree walk
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_output.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPA_OUTPUT_H
#define PGPA_OUTPUT_H

#include "pgpa_identifier.h"
#include "pgpa_walker.h"

extern void pgpa_output_advice(StringInfo buf,
							   pgpa_plan_walker_context *walker,
							   pgpa_identifier *rt_identifiers);

#endif
