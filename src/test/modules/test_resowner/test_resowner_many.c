/*--------------------------------------------------------------------------
 *
 * test_resowner_many.c
 *		Test ResourceOwner functionality with lots of resources
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_resowner/test_resowner_many.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/ilist.h"
#include "utils/resowner.h"

/*
 * Define a custom resource type to use in the test.  The resource being
 * tracked is a palloc'd ManyTestResource struct.
 *
 * To cross-check that the ResourceOwner calls the callback functions
 * correctly, we keep track of the remembered resources ourselves in a linked
 * list, and also keep counters of how many times the callback functions have
 * been called.
 */
typedef struct
{
	ResourceOwnerDesc desc;
	int			nremembered;
	int			nforgotten;
	int			nreleased;
	int			nleaked;

	dlist_head	current_resources;
} ManyTestResourceKind;

typedef struct
{
	ManyTestResourceKind *kind;
	dlist_node	node;
} ManyTestResource;

/*
 * Current release phase, and priority of last call to the release callback.
 * This is used to check that the resources are released in correct order.
 */
static ResourceReleasePhase current_release_phase;
static uint32 last_release_priority = 0;

/* prototypes for local functions */
static void ReleaseManyTestResource(Datum res);
static char *PrintManyTest(Datum res);
static void InitManyTestResourceKind(ManyTestResourceKind *kind, char *name,
									 ResourceReleasePhase phase, uint32 priority);
static void RememberManyTestResources(ResourceOwner owner,
									  ManyTestResourceKind *kinds, int nkinds,
									  int nresources);
static void ForgetManyTestResources(ResourceOwner owner,
									ManyTestResourceKind *kinds, int nkinds,
									int nresources);
static int	GetTotalResourceCount(ManyTestResourceKind *kinds, int nkinds);

/* ResourceOwner callback */
static void
ReleaseManyTestResource(Datum res)
{
	ManyTestResource *mres = (ManyTestResource *) DatumGetPointer(res);

	elog(DEBUG1, "releasing resource %p from %s", mres, mres->kind->desc.name);
	Assert(last_release_priority <= mres->kind->desc.release_priority);

	dlist_delete(&mres->node);
	mres->kind->nreleased++;
	last_release_priority = mres->kind->desc.release_priority;
	pfree(mres);
}

/* ResourceOwner callback */
static char *
PrintManyTest(Datum res)
{
	ManyTestResource *mres = (ManyTestResource *) DatumGetPointer(res);

	/*
	 * XXX: we assume that the DebugPrint function is called once for each
	 * leaked resource, and that there are no other callers.
	 */
	mres->kind->nleaked++;

	return psprintf("many-test resource from %s", mres->kind->desc.name);
}

static void
InitManyTestResourceKind(ManyTestResourceKind *kind, char *name,
						 ResourceReleasePhase phase, uint32 priority)
{
	kind->desc.name = name;
	kind->desc.release_phase = phase;
	kind->desc.release_priority = priority;
	kind->desc.ReleaseResource = ReleaseManyTestResource;
	kind->desc.DebugPrint = PrintManyTest;
	kind->nremembered = 0;
	kind->nforgotten = 0;
	kind->nreleased = 0;
	kind->nleaked = 0;
	dlist_init(&kind->current_resources);
}

/*
 * Remember 'nresources' resources.  The resources are remembered in round
 * robin fashion with the kinds from 'kinds' array.
 */
static void
RememberManyTestResources(ResourceOwner owner,
						  ManyTestResourceKind *kinds, int nkinds,
						  int nresources)
{
	int			kind_idx = 0;

	for (int i = 0; i < nresources; i++)
	{
		ManyTestResource *mres = palloc(sizeof(ManyTestResource));

		mres->kind = &kinds[kind_idx];
		dlist_node_init(&mres->node);

		ResourceOwnerEnlarge(owner);
		ResourceOwnerRemember(owner, PointerGetDatum(mres), &kinds[kind_idx].desc);
		kinds[kind_idx].nremembered++;
		dlist_push_tail(&kinds[kind_idx].current_resources, &mres->node);

		elog(DEBUG1, "remembered resource %p from %s", mres, mres->kind->desc.name);

		kind_idx = (kind_idx + 1) % nkinds;
	}
}

/*
 * Forget 'nresources' resources, in round robin fashion from 'kinds'.
 */
static void
ForgetManyTestResources(ResourceOwner owner,
						ManyTestResourceKind *kinds, int nkinds,
						int nresources)
{
	int			kind_idx = 0;
	int			ntotal;

	ntotal = GetTotalResourceCount(kinds, nkinds);
	if (ntotal < nresources)
		elog(PANIC, "cannot free %d resources, only %d remembered", nresources, ntotal);

	for (int i = 0; i < nresources; i++)
	{
		bool		found = false;

		for (int j = 0; j < nkinds; j++)
		{
			kind_idx = (kind_idx + 1) % nkinds;
			if (!dlist_is_empty(&kinds[kind_idx].current_resources))
			{
				ManyTestResource *mres = dlist_head_element(ManyTestResource, node, &kinds[kind_idx].current_resources);

				ResourceOwnerForget(owner, PointerGetDatum(mres), &kinds[kind_idx].desc);
				kinds[kind_idx].nforgotten++;
				dlist_delete(&mres->node);
				pfree(mres);

				found = true;
				break;
			}
		}
		if (!found)
			elog(ERROR, "could not find a test resource to forget");
	}
}

/*
 * Get total number of currently active resources among 'kinds'.
 */
static int
GetTotalResourceCount(ManyTestResourceKind *kinds, int nkinds)
{
	int			ntotal = 0;

	for (int i = 0; i < nkinds; i++)
		ntotal += kinds[i].nremembered - kinds[i].nforgotten - kinds[i].nreleased;

	return ntotal;
}

/*
 * Remember lots of resources, belonging to 'nkinds' different resource types
 * with different priorities.  Then forget some of them, and finally, release
 * the resource owner.  We use a custom resource type that performs various
 * sanity checks to verify that all the resources are released, and in the
 * correct order.
 */
PG_FUNCTION_INFO_V1(test_resowner_many);
Datum
test_resowner_many(PG_FUNCTION_ARGS)
{
	int32		nkinds = PG_GETARG_INT32(0);
	int32		nremember_bl = PG_GETARG_INT32(1);
	int32		nforget_bl = PG_GETARG_INT32(2);
	int32		nremember_al = PG_GETARG_INT32(3);
	int32		nforget_al = PG_GETARG_INT32(4);

	ResourceOwner resowner;

	ManyTestResourceKind *before_kinds;
	ManyTestResourceKind *after_kinds;

	/* Sanity check the arguments */
	if (nkinds < 0)
		elog(ERROR, "nkinds must be >= 0");
	if (nremember_bl < 0)
		elog(ERROR, "nremember_bl must be >= 0");
	if (nforget_bl < 0 || nforget_bl > nremember_bl)
		elog(ERROR, "nforget_bl must between 0 and 'nremember_bl'");
	if (nremember_al < 0)
		elog(ERROR, "nremember_al must be greater than zero");
	if (nforget_al < 0 || nforget_al > nremember_al)
		elog(ERROR, "nforget_al must between 0 and 'nremember_al'");

	/* Initialize all the different resource kinds to use */
	before_kinds = palloc(nkinds * sizeof(ManyTestResourceKind));
	for (int i = 0; i < nkinds; i++)
	{
		InitManyTestResourceKind(&before_kinds[i],
								 psprintf("resource before locks %d", i),
								 RESOURCE_RELEASE_BEFORE_LOCKS,
								 RELEASE_PRIO_FIRST + i);
	}
	after_kinds = palloc(nkinds * sizeof(ManyTestResourceKind));
	for (int i = 0; i < nkinds; i++)
	{
		InitManyTestResourceKind(&after_kinds[i],
								 psprintf("resource after locks %d", i),
								 RESOURCE_RELEASE_AFTER_LOCKS,
								 RELEASE_PRIO_FIRST + i);
	}

	resowner = ResourceOwnerCreate(CurrentResourceOwner, "TestOwner");

	/* Remember a bunch of resources */
	if (nremember_bl > 0)
	{
		elog(NOTICE, "remembering %d before-locks resources", nremember_bl);
		RememberManyTestResources(resowner, before_kinds, nkinds, nremember_bl);
	}
	if (nremember_al > 0)
	{
		elog(NOTICE, "remembering %d after-locks resources", nremember_al);
		RememberManyTestResources(resowner, after_kinds, nkinds, nremember_al);
	}

	/* Forget what was remembered */
	if (nforget_bl > 0)
	{
		elog(NOTICE, "forgetting %d before-locks resources", nforget_bl);
		ForgetManyTestResources(resowner, before_kinds, nkinds, nforget_bl);
	}

	if (nforget_al > 0)
	{
		elog(NOTICE, "forgetting %d after-locks resources", nforget_al);
		ForgetManyTestResources(resowner, after_kinds, nkinds, nforget_al);
	}

	/* Start releasing */
	elog(NOTICE, "releasing resources before locks");
	current_release_phase = RESOURCE_RELEASE_BEFORE_LOCKS;
	last_release_priority = 0;
	ResourceOwnerRelease(resowner, RESOURCE_RELEASE_BEFORE_LOCKS, false, false);
	Assert(GetTotalResourceCount(before_kinds, nkinds) == 0);

	elog(NOTICE, "releasing locks");
	current_release_phase = RESOURCE_RELEASE_LOCKS;
	last_release_priority = 0;
	ResourceOwnerRelease(resowner, RESOURCE_RELEASE_LOCKS, false, false);

	elog(NOTICE, "releasing resources after locks");
	current_release_phase = RESOURCE_RELEASE_AFTER_LOCKS;
	last_release_priority = 0;
	ResourceOwnerRelease(resowner, RESOURCE_RELEASE_AFTER_LOCKS, false, false);
	Assert(GetTotalResourceCount(before_kinds, nkinds) == 0);
	Assert(GetTotalResourceCount(after_kinds, nkinds) == 0);

	ResourceOwnerDelete(resowner);

	PG_RETURN_VOID();
}
