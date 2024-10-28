/*--------------------------------------------------------------------------
 *
 * test_resowner_basic.c
 *		Test basic ResourceOwner functionality
 *
 * Copyright (c) 2022-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_resowner/test_resowner_basic.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/resowner.h"

PG_MODULE_MAGIC;

static void ReleaseString(Datum res);
static char *PrintString(Datum res);

/*
 * A resource that tracks strings and prints the string when it's released.
 * This makes the order that the resources are released visible.
 */
static const ResourceOwnerDesc string_desc = {
	.name = "test resource",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_FIRST,
	.ReleaseResource = ReleaseString,
	.DebugPrint = PrintString
};

static void
ReleaseString(Datum res)
{
	elog(NOTICE, "releasing string: %s", DatumGetPointer(res));
}

static char *
PrintString(Datum res)
{
	return psprintf("test string \"%s\"", DatumGetPointer(res));
}

/* demonstrates phases and priorities between a parent and child context */
PG_FUNCTION_INFO_V1(test_resowner_priorities);
Datum
test_resowner_priorities(PG_FUNCTION_ARGS)
{
	int32		nkinds = PG_GETARG_INT32(0);
	int32		nresources = PG_GETARG_INT32(1);
	ResourceOwner parent,
				child;
	ResourceOwnerDesc *before_desc;
	ResourceOwnerDesc *after_desc;

	if (nkinds <= 0)
		elog(ERROR, "nkinds must be greater than zero");
	if (nresources <= 0)
		elog(ERROR, "nresources must be greater than zero");

	parent = ResourceOwnerCreate(CurrentResourceOwner, "test parent");
	child = ResourceOwnerCreate(parent, "test child");

	before_desc = palloc(nkinds * sizeof(ResourceOwnerDesc));
	for (int i = 0; i < nkinds; i++)
	{
		before_desc[i].name = psprintf("test resource before locks %d", i);
		before_desc[i].release_phase = RESOURCE_RELEASE_BEFORE_LOCKS;
		before_desc[i].release_priority = RELEASE_PRIO_FIRST + i;
		before_desc[i].ReleaseResource = ReleaseString;
		before_desc[i].DebugPrint = PrintString;
	}
	after_desc = palloc(nkinds * sizeof(ResourceOwnerDesc));
	for (int i = 0; i < nkinds; i++)
	{
		after_desc[i].name = psprintf("test resource after locks %d", i);
		after_desc[i].release_phase = RESOURCE_RELEASE_AFTER_LOCKS;
		after_desc[i].release_priority = RELEASE_PRIO_FIRST + i;
		after_desc[i].ReleaseResource = ReleaseString;
		after_desc[i].DebugPrint = PrintString;
	}

	/* Add a bunch of resources to child, with different priorities */
	for (int i = 0; i < nresources; i++)
	{
		ResourceOwnerDesc *kind = &before_desc[i % nkinds];

		ResourceOwnerEnlarge(child);
		ResourceOwnerRemember(child,
							  CStringGetDatum(psprintf("child before locks priority %d", kind->release_priority)),
							  kind);
	}
	for (int i = 0; i < nresources; i++)
	{
		ResourceOwnerDesc *kind = &after_desc[i % nkinds];

		ResourceOwnerEnlarge(child);
		ResourceOwnerRemember(child,
							  CStringGetDatum(psprintf("child after locks priority %d", kind->release_priority)),
							  kind);
	}

	/* And also to the parent */
	for (int i = 0; i < nresources; i++)
	{
		ResourceOwnerDesc *kind = &after_desc[i % nkinds];

		ResourceOwnerEnlarge(parent);
		ResourceOwnerRemember(parent,
							  CStringGetDatum(psprintf("parent after locks priority %d", kind->release_priority)),
							  kind);
	}
	for (int i = 0; i < nresources; i++)
	{
		ResourceOwnerDesc *kind = &before_desc[i % nkinds];

		ResourceOwnerEnlarge(parent);
		ResourceOwnerRemember(parent,
							  CStringGetDatum(psprintf("parent before locks priority %d", kind->release_priority)),
							  kind);
	}

	elog(NOTICE, "releasing resources before locks");
	ResourceOwnerRelease(parent, RESOURCE_RELEASE_BEFORE_LOCKS, false, false);
	elog(NOTICE, "releasing locks");
	ResourceOwnerRelease(parent, RESOURCE_RELEASE_LOCKS, false, false);
	elog(NOTICE, "releasing resources after locks");
	ResourceOwnerRelease(parent, RESOURCE_RELEASE_AFTER_LOCKS, false, false);

	ResourceOwnerDelete(parent);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_resowner_leak);
Datum
test_resowner_leak(PG_FUNCTION_ARGS)
{
	ResourceOwner resowner;

	resowner = ResourceOwnerCreate(CurrentResourceOwner, "TestOwner");

	ResourceOwnerEnlarge(resowner);

	ResourceOwnerRemember(resowner, CStringGetDatum("my string"), &string_desc);

	/* don't call ResourceOwnerForget, so that it is leaked */

	ResourceOwnerRelease(resowner, RESOURCE_RELEASE_BEFORE_LOCKS, true, false);
	ResourceOwnerRelease(resowner, RESOURCE_RELEASE_LOCKS, true, false);
	ResourceOwnerRelease(resowner, RESOURCE_RELEASE_AFTER_LOCKS, true, false);

	ResourceOwnerDelete(resowner);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_resowner_remember_between_phases);
Datum
test_resowner_remember_between_phases(PG_FUNCTION_ARGS)
{
	ResourceOwner resowner;

	resowner = ResourceOwnerCreate(CurrentResourceOwner, "TestOwner");

	ResourceOwnerRelease(resowner, RESOURCE_RELEASE_BEFORE_LOCKS, true, false);

	/*
	 * Try to remember a new resource.  Fails because we already called
	 * ResourceOwnerRelease.
	 */
	ResourceOwnerEnlarge(resowner);
	ResourceOwnerRemember(resowner, CStringGetDatum("my string"), &string_desc);

	/* unreachable */
	elog(ERROR, "ResourceOwnerEnlarge should have errored out");

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_resowner_forget_between_phases);
Datum
test_resowner_forget_between_phases(PG_FUNCTION_ARGS)
{
	ResourceOwner resowner;
	Datum		str_resource;

	resowner = ResourceOwnerCreate(CurrentResourceOwner, "TestOwner");

	ResourceOwnerEnlarge(resowner);
	str_resource = CStringGetDatum("my string");
	ResourceOwnerRemember(resowner, str_resource, &string_desc);

	ResourceOwnerRelease(resowner, RESOURCE_RELEASE_BEFORE_LOCKS, true, false);

	/*
	 * Try to forget the resource that was remembered earlier.  Fails because
	 * we already called ResourceOwnerRelease.
	 */
	ResourceOwnerForget(resowner, str_resource, &string_desc);

	/* unreachable */
	elog(ERROR, "ResourceOwnerForget should have errored out");

	PG_RETURN_VOID();
}
