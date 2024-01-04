/*-------------------------------------------------------------------------
 *
 * session.c
 *		Encapsulation of user session.
 *
 * This is intended to contain data that needs to be shared between backends
 * performing work for a client session.  In particular such a session is
 * shared between the leader and worker processes for parallel queries.  At
 * some later point it might also become useful infrastructure for separating
 * backends from client connections, e.g. for the purpose of pooling.
 *
 * Currently this infrastructure is used to share:
 * - typemod registry for ephemeral row-types, i.e. BlessTupleDesc etc.
 *
 * Portions Copyright (c) 2017-2024, PostgreSQL Global Development Group
 *
 * src/backend/access/common/session.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/session.h"
#include "storage/lwlock.h"
#include "storage/shm_toc.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

/* Magic number for per-session DSM TOC. */
#define SESSION_MAGIC						0xabb0fbc9

/*
 * We want to create a DSA area to store shared state that has the same
 * lifetime as a session.  So far, it's only used to hold the shared record
 * type registry.  We don't want it to have to create any DSM segments just
 * yet in common cases, so we'll give it enough space to hold a very small
 * SharedRecordTypmodRegistry.
 */
#define SESSION_DSA_SIZE					0x30000

/*
 * Magic numbers for state sharing in the per-session DSM area.
 */
#define SESSION_KEY_DSA						UINT64CONST(0xFFFFFFFFFFFF0001)
#define SESSION_KEY_RECORD_TYPMOD_REGISTRY	UINT64CONST(0xFFFFFFFFFFFF0002)

/* This backend's current session. */
Session    *CurrentSession = NULL;

/*
 * Set up CurrentSession to point to an empty Session object.
 */
void
InitializeSession(void)
{
	CurrentSession = MemoryContextAllocZero(TopMemoryContext, sizeof(Session));
}

/*
 * Initialize the per-session DSM segment if it isn't already initialized, and
 * return its handle so that worker processes can attach to it.
 *
 * Unlike the per-context DSM segment, this segment and its contents are
 * reused for future parallel queries.
 *
 * Return DSM_HANDLE_INVALID if a segment can't be allocated due to lack of
 * resources.
 */
dsm_handle
GetSessionDsmHandle(void)
{
	shm_toc_estimator estimator;
	shm_toc    *toc;
	dsm_segment *seg;
	size_t		typmod_registry_size;
	size_t		size;
	void	   *dsa_space;
	void	   *typmod_registry_space;
	dsa_area   *dsa;
	MemoryContext old_context;

	/*
	 * If we have already created a session-scope DSM segment in this backend,
	 * return its handle.  The same segment will be used for the rest of this
	 * backend's lifetime.
	 */
	if (CurrentSession->segment != NULL)
		return dsm_segment_handle(CurrentSession->segment);

	/* Otherwise, prepare to set one up. */
	old_context = MemoryContextSwitchTo(TopMemoryContext);
	shm_toc_initialize_estimator(&estimator);

	/* Estimate space for the per-session DSA area. */
	shm_toc_estimate_keys(&estimator, 1);
	shm_toc_estimate_chunk(&estimator, SESSION_DSA_SIZE);

	/* Estimate space for the per-session record typmod registry. */
	typmod_registry_size = SharedRecordTypmodRegistryEstimate();
	shm_toc_estimate_keys(&estimator, 1);
	shm_toc_estimate_chunk(&estimator, typmod_registry_size);

	/* Set up segment and TOC. */
	size = shm_toc_estimate(&estimator);
	seg = dsm_create(size, DSM_CREATE_NULL_IF_MAXSEGMENTS);
	if (seg == NULL)
	{
		MemoryContextSwitchTo(old_context);

		return DSM_HANDLE_INVALID;
	}
	toc = shm_toc_create(SESSION_MAGIC,
						 dsm_segment_address(seg),
						 size);

	/* Create per-session DSA area. */
	dsa_space = shm_toc_allocate(toc, SESSION_DSA_SIZE);
	dsa = dsa_create_in_place(dsa_space,
							  SESSION_DSA_SIZE,
							  LWTRANCHE_PER_SESSION_DSA,
							  seg);
	shm_toc_insert(toc, SESSION_KEY_DSA, dsa_space);


	/* Create session-scoped shared record typmod registry. */
	typmod_registry_space = shm_toc_allocate(toc, typmod_registry_size);
	SharedRecordTypmodRegistryInit((SharedRecordTypmodRegistry *)
								   typmod_registry_space, seg, dsa);
	shm_toc_insert(toc, SESSION_KEY_RECORD_TYPMOD_REGISTRY,
				   typmod_registry_space);

	/*
	 * If we got this far, we can pin the shared memory so it stays mapped for
	 * the rest of this backend's life.  If we don't make it this far, cleanup
	 * callbacks for anything we installed above (ie currently
	 * SharedRecordTypmodRegistry) will run when the DSM segment is detached
	 * by CurrentResourceOwner so we aren't left with a broken CurrentSession.
	 */
	dsm_pin_mapping(seg);
	dsa_pin_mapping(dsa);

	/* Make segment and area available via CurrentSession. */
	CurrentSession->segment = seg;
	CurrentSession->area = dsa;

	MemoryContextSwitchTo(old_context);

	return dsm_segment_handle(seg);
}

/*
 * Attach to a per-session DSM segment provided by a parallel leader.
 */
void
AttachSession(dsm_handle handle)
{
	dsm_segment *seg;
	shm_toc    *toc;
	void	   *dsa_space;
	void	   *typmod_registry_space;
	dsa_area   *dsa;
	MemoryContext old_context;

	old_context = MemoryContextSwitchTo(TopMemoryContext);

	/* Attach to the DSM segment. */
	seg = dsm_attach(handle);
	if (seg == NULL)
		elog(ERROR, "could not attach to per-session DSM segment");
	toc = shm_toc_attach(SESSION_MAGIC, dsm_segment_address(seg));

	/* Attach to the DSA area. */
	dsa_space = shm_toc_lookup(toc, SESSION_KEY_DSA, false);
	dsa = dsa_attach_in_place(dsa_space, seg);

	/* Make them available via the current session. */
	CurrentSession->segment = seg;
	CurrentSession->area = dsa;

	/* Attach to the shared record typmod registry. */
	typmod_registry_space =
		shm_toc_lookup(toc, SESSION_KEY_RECORD_TYPMOD_REGISTRY, false);
	SharedRecordTypmodRegistryAttach((SharedRecordTypmodRegistry *)
									 typmod_registry_space);

	/* Remain attached until end of backend or DetachSession(). */
	dsm_pin_mapping(seg);
	dsa_pin_mapping(dsa);

	MemoryContextSwitchTo(old_context);
}

/*
 * Detach from the current session DSM segment.  It's not strictly necessary
 * to do this explicitly since we'll detach automatically at backend exit, but
 * if we ever reuse parallel workers it will become important for workers to
 * detach from one session before attaching to another.  Note that this runs
 * detach hooks.
 */
void
DetachSession(void)
{
	/* Runs detach hooks. */
	dsm_detach(CurrentSession->segment);
	CurrentSession->segment = NULL;
	dsa_detach(CurrentSession->area);
	CurrentSession->area = NULL;
}
