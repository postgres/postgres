/*-------------------------------------------------------------------------
 *
 * extendplan.h
 *	  Extend core planner objects with additional private state
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/extendplan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTENDPLAN_H
#define EXTENDPLAN_H

#include "nodes/pathnodes.h"

extern int	GetPlannerExtensionId(const char *extension_name);

/*
 * Get extension-specific state from a PlannerGlobal.
 */
static inline void *
GetPlannerGlobalExtensionState(PlannerGlobal *glob, int extension_id)
{
	Assert(extension_id >= 0);

	if (extension_id >= glob->extension_state_allocated)
		return NULL;

	return glob->extension_state[extension_id];
}

/*
 * Get extension-specific state from a PlannerInfo.
 */
static inline void *
GetPlannerInfoExtensionState(PlannerInfo *root, int extension_id)
{
	Assert(extension_id >= 0);

	if (extension_id >= root->extension_state_allocated)
		return NULL;

	return root->extension_state[extension_id];
}

/*
 * Get extension-specific state from a PlannerInfo.
 */
static inline void *
GetRelOptInfoExtensionState(RelOptInfo *rel, int extension_id)
{
	Assert(extension_id >= 0);

	if (extension_id >= rel->extension_state_allocated)
		return NULL;

	return rel->extension_state[extension_id];
}

/* Functions to store private state into various planner objects */
extern void SetPlannerGlobalExtensionState(PlannerGlobal *glob,
										   int extension_id,
										   void *opaque);
extern void SetPlannerInfoExtensionState(PlannerInfo *root, int extension_id,
										 void *opaque);
extern void SetRelOptInfoExtensionState(RelOptInfo *rel, int extension_id,
										void *opaque);

#endif
