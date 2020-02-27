/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/uavc.c
 *
 * Implementation of userspace access vector cache; that enables to cache
 * access control decisions recently used, and reduce number of kernel
 * invocations to avoid unnecessary performance hit.
 *
 * Copyright (c) 2011-2020, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_proc.h"
#include "commands/seclabel.h"
#include "common/hashfn.h"
#include "sepgsql.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/memutils.h"

/*
 * avc_cache
 *
 * It enables to cache access control decision (and behavior on execution of
 * trusted procedure, db_procedure class only) for a particular pair of
 * security labels and object class in userspace.
 */
typedef struct
{
	uint32		hash;			/* hash value of this cache entry */
	char	   *scontext;		/* security context of the subject */
	char	   *tcontext;		/* security context of the target */
	uint16		tclass;			/* object class of the target */

	uint32		allowed;		/* permissions to be allowed */
	uint32		auditallow;		/* permissions to be audited on allowed */
	uint32		auditdeny;		/* permissions to be audited on denied */

	bool		permissive;		/* true, if permissive rule */
	bool		hot_cache;		/* true, if recently referenced */
	bool		tcontext_is_valid;
	/* true, if tcontext is valid */
	char	   *ncontext;		/* temporary scontext on execution of trusted
								 * procedure, or NULL elsewhere */
}			avc_cache;

/*
 * Declaration of static variables
 */
#define AVC_NUM_SLOTS		512
#define AVC_NUM_RECLAIM		16
#define AVC_DEF_THRESHOLD	384

static MemoryContext avc_mem_cxt;
static List *avc_slots[AVC_NUM_SLOTS];	/* avc's hash buckets */
static int	avc_num_caches;		/* number of caches currently used */
static int	avc_lru_hint;		/* index of the buckets to be reclaimed next */
static int	avc_threshold;		/* threshold to launch cache-reclaiming  */
static char *avc_unlabeled;		/* system 'unlabeled' label */

/*
 * Hash function
 */
static uint32
sepgsql_avc_hash(const char *scontext, const char *tcontext, uint16 tclass)
{
	return hash_any((const unsigned char *) scontext, strlen(scontext))
		^ hash_any((const unsigned char *) tcontext, strlen(tcontext))
		^ tclass;
}

/*
 * Reset all the avc caches
 */
static void
sepgsql_avc_reset(void)
{
	MemoryContextReset(avc_mem_cxt);

	memset(avc_slots, 0, sizeof(List *) * AVC_NUM_SLOTS);
	avc_num_caches = 0;
	avc_lru_hint = 0;
	avc_unlabeled = NULL;
}

/*
 * Reclaim caches recently unreferenced
 */
static void
sepgsql_avc_reclaim(void)
{
	ListCell   *cell;
	int			index;

	while (avc_num_caches >= avc_threshold - AVC_NUM_RECLAIM)
	{
		index = avc_lru_hint;

		foreach(cell, avc_slots[index])
		{
			avc_cache  *cache = lfirst(cell);

			if (!cache->hot_cache)
			{
				avc_slots[index]
					= foreach_delete_current(avc_slots[index], cell);

				pfree(cache->scontext);
				pfree(cache->tcontext);
				if (cache->ncontext)
					pfree(cache->ncontext);
				pfree(cache);

				avc_num_caches--;
			}
			else
			{
				cache->hot_cache = false;
			}
		}
		avc_lru_hint = (avc_lru_hint + 1) % AVC_NUM_SLOTS;
	}
}

/* -------------------------------------------------------------------------
 *
 * sepgsql_avc_check_valid
 *
 * This function checks whether the cached entries are still valid.  If
 * the security policy has been reloaded (or any other events that requires
 * resetting userspace caches has occurred) since the last reference to
 * the access vector cache, we must flush the cache.
 *
 * Access control decisions must be atomic, but multiple system calls may
 * be required to make a decision; thus, when referencing the access vector
 * cache, we must loop until we complete without an intervening cache flush
 * event.  In practice, looping even once should be very rare.  Callers should
 * do something like this:
 *
 *	 sepgsql_avc_check_valid();
 *	 do {
 *			 :
 *		 <reference to uavc>
 *			 :
 *	 } while (!sepgsql_avc_check_valid())
 *
 * -------------------------------------------------------------------------
 */
static bool
sepgsql_avc_check_valid(void)
{
	if (selinux_status_updated() > 0)
	{
		sepgsql_avc_reset();

		return false;
	}
	return true;
}

/*
 * sepgsql_avc_unlabeled
 *
 * Returns an alternative label to be applied when no label or an invalid
 * label would otherwise be assigned.
 */
static char *
sepgsql_avc_unlabeled(void)
{
	if (!avc_unlabeled)
	{
		security_context_t unlabeled;

		if (security_get_initial_context_raw("unlabeled", &unlabeled) < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SELinux: failed to get initial security label: %m")));
		PG_TRY();
		{
			avc_unlabeled = MemoryContextStrdup(avc_mem_cxt, unlabeled);
		}
		PG_FINALLY();
		{
			freecon(unlabeled);
		}
		PG_END_TRY();
	}
	return avc_unlabeled;
}

/*
 * sepgsql_avc_compute
 *
 * A fallback path, when cache mishit. It asks SELinux its access control
 * decision for the supplied pair of security context and object class.
 */
static avc_cache *
sepgsql_avc_compute(const char *scontext, const char *tcontext, uint16 tclass)
{
	char	   *ucontext = NULL;
	char	   *ncontext = NULL;
	MemoryContext oldctx;
	avc_cache  *cache;
	uint32		hash;
	int			index;
	struct av_decision avd;

	hash = sepgsql_avc_hash(scontext, tcontext, tclass);
	index = hash % AVC_NUM_SLOTS;

	/*
	 * Validation check of the supplied security context. Because it always
	 * invoke system-call, frequent check should be avoided. Unless security
	 * policy is reloaded, validation status shall be kept, so we also cache
	 * whether the supplied security context was valid, or not.
	 */
	if (security_check_context_raw((security_context_t) tcontext) != 0)
		ucontext = sepgsql_avc_unlabeled();

	/*
	 * Ask SELinux its access control decision
	 */
	if (!ucontext)
		sepgsql_compute_avd(scontext, tcontext, tclass, &avd);
	else
		sepgsql_compute_avd(scontext, ucontext, tclass, &avd);

	/*
	 * It also caches a security label to be switched when a client labeled as
	 * 'scontext' executes a procedure labeled as 'tcontext', not only access
	 * control decision on the procedure. The security label to be switched
	 * shall be computed uniquely on a pair of 'scontext' and 'tcontext',
	 * thus, it is reasonable to cache the new label on avc, and enables to
	 * reduce unnecessary system calls. It shall be referenced at
	 * sepgsql_needs_fmgr_hook to check whether the supplied function is a
	 * trusted procedure, or not.
	 */
	if (tclass == SEPG_CLASS_DB_PROCEDURE)
	{
		if (!ucontext)
			ncontext = sepgsql_compute_create(scontext, tcontext,
											  SEPG_CLASS_PROCESS, NULL);
		else
			ncontext = sepgsql_compute_create(scontext, ucontext,
											  SEPG_CLASS_PROCESS, NULL);
		if (strcmp(scontext, ncontext) == 0)
		{
			pfree(ncontext);
			ncontext = NULL;
		}
	}

	/*
	 * Set up an avc_cache object
	 */
	oldctx = MemoryContextSwitchTo(avc_mem_cxt);

	cache = palloc0(sizeof(avc_cache));

	cache->hash = hash;
	cache->scontext = pstrdup(scontext);
	cache->tcontext = pstrdup(tcontext);
	cache->tclass = tclass;

	cache->allowed = avd.allowed;
	cache->auditallow = avd.auditallow;
	cache->auditdeny = avd.auditdeny;
	cache->hot_cache = true;
	if (avd.flags & SELINUX_AVD_FLAGS_PERMISSIVE)
		cache->permissive = true;
	if (!ucontext)
		cache->tcontext_is_valid = true;
	if (ncontext)
		cache->ncontext = pstrdup(ncontext);

	avc_num_caches++;

	if (avc_num_caches > avc_threshold)
		sepgsql_avc_reclaim();

	avc_slots[index] = lcons(cache, avc_slots[index]);

	MemoryContextSwitchTo(oldctx);

	return cache;
}

/*
 * sepgsql_avc_lookup
 *
 * Look up a cache entry that matches the supplied security contexts and
 * object class.  If not found, create a new cache entry.
 */
static avc_cache *
sepgsql_avc_lookup(const char *scontext, const char *tcontext, uint16 tclass)
{
	avc_cache  *cache;
	ListCell   *cell;
	uint32		hash;
	int			index;

	hash = sepgsql_avc_hash(scontext, tcontext, tclass);
	index = hash % AVC_NUM_SLOTS;

	foreach(cell, avc_slots[index])
	{
		cache = lfirst(cell);

		if (cache->hash == hash &&
			cache->tclass == tclass &&
			strcmp(cache->tcontext, tcontext) == 0 &&
			strcmp(cache->scontext, scontext) == 0)
		{
			cache->hot_cache = true;
			return cache;
		}
	}
	/* not found, so insert a new cache */
	return sepgsql_avc_compute(scontext, tcontext, tclass);
}

/*
 * sepgsql_avc_check_perms(_label)
 *
 * It returns 'true', if the security policy suggested to allow the required
 * permissions. Otherwise, it returns 'false' or raises an error according
 * to the 'abort_on_violation' argument.
 * The 'tobject' and 'tclass' identify the target object being referenced,
 * and 'required' is a bitmask of permissions (SEPG_*__*) defined for each
 * object classes.
 * The 'audit_name' is the object name (optional). If SEPGSQL_AVC_NOAUDIT
 * was supplied, it means to skip all the audit messages.
 */
bool
sepgsql_avc_check_perms_label(const char *tcontext,
							  uint16 tclass, uint32 required,
							  const char *audit_name,
							  bool abort_on_violation)
{
	char	   *scontext = sepgsql_get_client_label();
	avc_cache  *cache;
	uint32		denied;
	uint32		audited;
	bool		result;

	sepgsql_avc_check_valid();
	do
	{
		result = true;

		/*
		 * If the target object is unlabeled, we perform the check using the
		 * label supplied by sepgsql_avc_unlabeled().
		 */
		if (tcontext)
			cache = sepgsql_avc_lookup(scontext, tcontext, tclass);
		else
			cache = sepgsql_avc_lookup(scontext,
									   sepgsql_avc_unlabeled(), tclass);

		denied = required & ~cache->allowed;

		/*
		 * Compute permissions to be audited
		 */
		if (sepgsql_get_debug_audit())
			audited = (denied ? (denied & ~0) : (required & ~0));
		else
			audited = denied ? (denied & cache->auditdeny)
				: (required & cache->auditallow);

		if (denied)
		{
			/*
			 * In permissive mode or permissive domain, violated permissions
			 * shall be audited to the log files at once, and then implicitly
			 * allowed to avoid a flood of access denied logs, because the
			 * purpose of permissive mode/domain is to collect a violation log
			 * that will make it possible to fix up the security policy.
			 */
			if (!sepgsql_getenforce() || cache->permissive)
				cache->allowed |= required;
			else
				result = false;
		}
	} while (!sepgsql_avc_check_valid());

	/*
	 * In the case when we have something auditable actions here,
	 * sepgsql_audit_log shall be called with text representation of security
	 * labels for both of subject and object. It records this access
	 * violation, so DBA will be able to find out unexpected security problems
	 * later.
	 */
	if (audited != 0 &&
		audit_name != SEPGSQL_AVC_NOAUDIT &&
		sepgsql_get_mode() != SEPGSQL_MODE_INTERNAL)
	{
		sepgsql_audit_log(denied != 0,
						  cache->scontext,
						  cache->tcontext_is_valid ?
						  cache->tcontext : sepgsql_avc_unlabeled(),
						  cache->tclass,
						  audited,
						  audit_name);
	}

	if (abort_on_violation && !result)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("SELinux: security policy violation")));

	return result;
}

bool
sepgsql_avc_check_perms(const ObjectAddress *tobject,
						uint16 tclass, uint32 required,
						const char *audit_name,
						bool abort_on_violation)
{
	char	   *tcontext = GetSecurityLabel(tobject, SEPGSQL_LABEL_TAG);
	bool		rc;

	rc = sepgsql_avc_check_perms_label(tcontext,
									   tclass, required,
									   audit_name, abort_on_violation);
	if (tcontext)
		pfree(tcontext);

	return rc;
}

/*
 * sepgsql_avc_trusted_proc
 *
 * If the supplied function OID is configured as a trusted procedure, this
 * function will return a security label to be used during the execution of
 * that function.  Otherwise, it returns NULL.
 */
char *
sepgsql_avc_trusted_proc(Oid functionId)
{
	char	   *scontext = sepgsql_get_client_label();
	char	   *tcontext;
	ObjectAddress tobject;
	avc_cache  *cache;

	tobject.classId = ProcedureRelationId;
	tobject.objectId = functionId;
	tobject.objectSubId = 0;
	tcontext = GetSecurityLabel(&tobject, SEPGSQL_LABEL_TAG);

	sepgsql_avc_check_valid();
	do
	{
		if (tcontext)
			cache = sepgsql_avc_lookup(scontext, tcontext,
									   SEPG_CLASS_DB_PROCEDURE);
		else
			cache = sepgsql_avc_lookup(scontext, sepgsql_avc_unlabeled(),
									   SEPG_CLASS_DB_PROCEDURE);
	} while (!sepgsql_avc_check_valid());

	return cache->ncontext;
}

/*
 * sepgsql_avc_exit
 *
 * Clean up userspace AVC on process exit.
 */
static void
sepgsql_avc_exit(int code, Datum arg)
{
	selinux_status_close();
}

/*
 * sepgsql_avc_init
 *
 * Initialize the userspace AVC.  This should be called from _PG_init.
 */
void
sepgsql_avc_init(void)
{
	int			rc;

	/*
	 * All the avc stuff shall be allocated in avc_mem_cxt
	 */
	avc_mem_cxt = AllocSetContextCreate(TopMemoryContext,
										"userspace access vector cache",
										ALLOCSET_DEFAULT_SIZES);
	memset(avc_slots, 0, sizeof(avc_slots));
	avc_num_caches = 0;
	avc_lru_hint = 0;
	avc_threshold = AVC_DEF_THRESHOLD;

	/*
	 * SELinux allows to mmap(2) its kernel status page in read-only mode to
	 * inform userspace applications its status updating (such as policy
	 * reloading) without system-call invocations. This feature is only
	 * supported in Linux-2.6.38 or later, however, libselinux provides a
	 * fallback mode to know its status using netlink sockets.
	 */
	rc = selinux_status_open(1);
	if (rc < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: could not open selinux status : %m")));
	else if (rc > 0)
		ereport(LOG,
				(errmsg("SELinux: kernel status page uses fallback mode")));

	/* Arrange to close selinux status page on process exit. */
	on_proc_exit(sepgsql_avc_exit, 0);
}
