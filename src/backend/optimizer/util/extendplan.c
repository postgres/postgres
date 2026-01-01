/*-------------------------------------------------------------------------
 *
 * extendplan.c
 *	  Extend core planner objects with additional private state
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * The interfaces defined in this file make it possible for loadable
 * modules to store their own private state inside of key planner data
 * structures -- specifically, the PlannerGlobal, PlannerInfo, and
 * RelOptInfo structures. This can make it much easier to write
 * reasonably efficient planner extensions; for instance, code that
 * uses set_join_pathlist_hook can arrange to compute a key intermediate
 * result once per joinrel rather than on every call.
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/extendplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/extendplan.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"

static const char **PlannerExtensionNameArray = NULL;
static int	PlannerExtensionNamesAssigned = 0;
static int	PlannerExtensionNamesAllocated = 0;

/*
 * Map the name of a planner extension to an integer ID.
 *
 * Within the lifetime of a particular backend, the same name will be mapped
 * to the same ID every time. IDs are not stable across backends. Use the ID
 * that you get from this function to call the remaining functions in this
 * file.
 */
int
GetPlannerExtensionId(const char *extension_name)
{
	/* Search for an existing extension by this name; if found, return ID. */
	for (int i = 0; i < PlannerExtensionNamesAssigned; ++i)
		if (strcmp(PlannerExtensionNameArray[i], extension_name) == 0)
			return i;

	/* If there is no array yet, create one. */
	if (PlannerExtensionNameArray == NULL)
	{
		PlannerExtensionNamesAllocated = 16;
		PlannerExtensionNameArray = (const char **)
			MemoryContextAlloc(TopMemoryContext,
							   PlannerExtensionNamesAllocated
							   * sizeof(char *));
	}

	/* If there's an array but it's currently full, expand it. */
	if (PlannerExtensionNamesAssigned >= PlannerExtensionNamesAllocated)
	{
		int			i = pg_nextpower2_32(PlannerExtensionNamesAssigned + 1);

		PlannerExtensionNameArray = (const char **)
			repalloc(PlannerExtensionNameArray, i * sizeof(char *));
		PlannerExtensionNamesAllocated = i;
	}

	/* Assign and return new ID. */
	PlannerExtensionNameArray[PlannerExtensionNamesAssigned] = extension_name;
	return PlannerExtensionNamesAssigned++;
}

/*
 * Store extension-specific state into a PlannerGlobal.
 */
void
SetPlannerGlobalExtensionState(PlannerGlobal *glob, int extension_id,
							   void *opaque)
{
	Assert(extension_id >= 0);

	/* If there is no array yet, create one. */
	if (glob->extension_state == NULL)
	{
		MemoryContext planner_cxt;
		Size		sz;

		planner_cxt = GetMemoryChunkContext(glob);
		glob->extension_state_allocated =
			Max(4, pg_nextpower2_32(extension_id + 1));
		sz = glob->extension_state_allocated * sizeof(void *);
		glob->extension_state = MemoryContextAllocZero(planner_cxt, sz);
	}

	/* If there's an array but it's currently full, expand it. */
	if (extension_id >= glob->extension_state_allocated)
	{
		int			i;

		i = pg_nextpower2_32(extension_id + 1);
		glob->extension_state = repalloc0_array(glob->extension_state, void *,
												glob->extension_state_allocated, i);
		glob->extension_state_allocated = i;
	}

	glob->extension_state[extension_id] = opaque;
}

/*
 * Store extension-specific state into a PlannerInfo.
 */
void
SetPlannerInfoExtensionState(PlannerInfo *root, int extension_id,
							 void *opaque)
{
	Assert(extension_id >= 0);

	/* If there is no array yet, create one. */
	if (root->extension_state == NULL)
	{
		Size		sz;

		root->extension_state_allocated =
			Max(4, pg_nextpower2_32(extension_id + 1));
		sz = root->extension_state_allocated * sizeof(void *);
		root->extension_state = MemoryContextAllocZero(root->planner_cxt, sz);
	}

	/* If there's an array but it's currently full, expand it. */
	if (extension_id >= root->extension_state_allocated)
	{
		int			i;

		i = pg_nextpower2_32(extension_id + 1);
		root->extension_state = repalloc0_array(root->extension_state, void *,
												root->extension_state_allocated, i);
		root->extension_state_allocated = i;
	}

	root->extension_state[extension_id] = opaque;
}

/*
 * Store extension-specific state into a RelOptInfo.
 */
void
SetRelOptInfoExtensionState(RelOptInfo *rel, int extension_id,
							void *opaque)
{
	Assert(extension_id >= 0);

	/* If there is no array yet, create one. */
	if (rel->extension_state == NULL)
	{
		MemoryContext planner_cxt;
		Size		sz;

		planner_cxt = GetMemoryChunkContext(rel);
		rel->extension_state_allocated =
			Max(4, pg_nextpower2_32(extension_id + 1));
		sz = rel->extension_state_allocated * sizeof(void *);
		rel->extension_state = MemoryContextAllocZero(planner_cxt, sz);
	}

	/* If there's an array but it's currently full, expand it. */
	if (extension_id >= rel->extension_state_allocated)
	{
		int			i;

		i = pg_nextpower2_32(extension_id + 1);
		rel->extension_state = repalloc0_array(rel->extension_state, void *,
											   rel->extension_state_allocated, i);
		rel->extension_state_allocated = i;
	}

	rel->extension_state[extension_id] = opaque;
}
