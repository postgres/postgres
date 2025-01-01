/*-------------------------------------------------------------------------
 *
 * expandedrecord.c
 *	  Functions for manipulating composite expanded objects.
 *
 * This module supports "expanded objects" (cf. expandeddatum.h) that can
 * store values of named composite types, domains over named composite types,
 * and record types (registered or anonymous).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/expandedrecord.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "catalog/heap.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/expandedrecord.h"
#include "utils/memutils.h"
#include "utils/typcache.h"


/* "Methods" required for an expanded object */
static Size ER_get_flat_size(ExpandedObjectHeader *eohptr);
static void ER_flatten_into(ExpandedObjectHeader *eohptr,
							void *result, Size allocated_size);

static const ExpandedObjectMethods ER_methods =
{
	ER_get_flat_size,
	ER_flatten_into
};

/* Other local functions */
static void ER_mc_callback(void *arg);
static MemoryContext get_short_term_cxt(ExpandedRecordHeader *erh);
static void build_dummy_expanded_header(ExpandedRecordHeader *main_erh);
static pg_noinline void check_domain_for_new_field(ExpandedRecordHeader *erh,
												   int fnumber,
												   Datum newValue, bool isnull);
static pg_noinline void check_domain_for_new_tuple(ExpandedRecordHeader *erh,
												   HeapTuple tuple);


/*
 * Build an expanded record of the specified composite type
 *
 * type_id can be RECORDOID, but only if a positive typmod is given.
 *
 * The expanded record is initially "empty", having a state logically
 * equivalent to a NULL composite value (not ROW(NULL, NULL, ...)).
 * Note that this might not be a valid state for a domain type;
 * if the caller needs to check that, call
 * expanded_record_set_tuple(erh, NULL, false, false).
 *
 * The expanded object will be a child of parentcontext.
 */
ExpandedRecordHeader *
make_expanded_record_from_typeid(Oid type_id, int32 typmod,
								 MemoryContext parentcontext)
{
	ExpandedRecordHeader *erh;
	int			flags = 0;
	TupleDesc	tupdesc;
	uint64		tupdesc_id;
	MemoryContext objcxt;
	char	   *chunk;

	if (type_id != RECORDOID)
	{
		/*
		 * Consult the typcache to see if it's a domain over composite, and in
		 * any case to get the tupdesc and tupdesc identifier.
		 */
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(type_id,
									 TYPECACHE_TUPDESC |
									 TYPECACHE_DOMAIN_BASE_INFO);
		if (typentry->typtype == TYPTYPE_DOMAIN)
		{
			flags |= ER_FLAG_IS_DOMAIN;
			typentry = lookup_type_cache(typentry->domainBaseType,
										 TYPECACHE_TUPDESC);
		}
		if (typentry->tupDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(type_id))));
		tupdesc = typentry->tupDesc;
		tupdesc_id = typentry->tupDesc_identifier;
	}
	else
	{
		/*
		 * For RECORD types, get the tupdesc and identifier from typcache.
		 */
		tupdesc = lookup_rowtype_tupdesc(type_id, typmod);
		tupdesc_id = assign_record_type_identifier(type_id, typmod);
	}

	/*
	 * Allocate private context for expanded object.  We use a regular-size
	 * context, not a small one, to improve the odds that we can fit a tupdesc
	 * into it without needing an extra malloc block.  (This code path doesn't
	 * ever need to copy a tupdesc into the expanded record, but let's be
	 * consistent with the other ways of making an expanded record.)
	 */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded record",
								   ALLOCSET_DEFAULT_SIZES);

	/*
	 * Since we already know the number of fields in the tupdesc, we can
	 * allocate the dvalues/dnulls arrays along with the record header.  This
	 * is useless if we never need those arrays, but it costs almost nothing,
	 * and it will save a palloc cycle if we do need them.
	 */
	erh = (ExpandedRecordHeader *)
		MemoryContextAlloc(objcxt, MAXALIGN(sizeof(ExpandedRecordHeader))
						   + tupdesc->natts * (sizeof(Datum) + sizeof(bool)));

	/* Ensure all header fields are initialized to 0/null */
	memset(erh, 0, sizeof(ExpandedRecordHeader));

	EOH_init_header(&erh->hdr, &ER_methods, objcxt);
	erh->er_magic = ER_MAGIC;

	/* Set up dvalues/dnulls, with no valid contents as yet */
	chunk = (char *) erh + MAXALIGN(sizeof(ExpandedRecordHeader));
	erh->dvalues = (Datum *) chunk;
	erh->dnulls = (bool *) (chunk + tupdesc->natts * sizeof(Datum));
	erh->nfields = tupdesc->natts;

	/* Fill in composite-type identification info */
	erh->er_decltypeid = type_id;
	erh->er_typeid = tupdesc->tdtypeid;
	erh->er_typmod = tupdesc->tdtypmod;
	erh->er_tupdesc_id = tupdesc_id;

	erh->flags = flags;

	/*
	 * If what we got from the typcache is a refcounted tupdesc, we need to
	 * acquire our own refcount on it.  We manage the refcount with a memory
	 * context callback rather than assuming that the CurrentResourceOwner is
	 * longer-lived than this expanded object.
	 */
	if (tupdesc->tdrefcount >= 0)
	{
		/* Register callback to release the refcount */
		erh->er_mcb.func = ER_mc_callback;
		erh->er_mcb.arg = erh;
		MemoryContextRegisterResetCallback(erh->hdr.eoh_context,
										   &erh->er_mcb);

		/* And save the pointer */
		erh->er_tupdesc = tupdesc;
		tupdesc->tdrefcount++;

		/* If we called lookup_rowtype_tupdesc, release the pin it took */
		if (type_id == RECORDOID)
			ReleaseTupleDesc(tupdesc);
	}
	else
	{
		/*
		 * If it's not refcounted, just assume it will outlive the expanded
		 * object.  (This can happen for shared record types, for instance.)
		 */
		erh->er_tupdesc = tupdesc;
	}

	/*
	 * We don't set ER_FLAG_DVALUES_VALID or ER_FLAG_FVALUE_VALID, so the
	 * record remains logically empty.
	 */

	return erh;
}

/*
 * Build an expanded record of the rowtype defined by the tupdesc
 *
 * The tupdesc is copied if necessary (i.e., if we can't just bump its
 * reference count instead).
 *
 * The expanded record is initially "empty", having a state logically
 * equivalent to a NULL composite value (not ROW(NULL, NULL, ...)).
 *
 * The expanded object will be a child of parentcontext.
 */
ExpandedRecordHeader *
make_expanded_record_from_tupdesc(TupleDesc tupdesc,
								  MemoryContext parentcontext)
{
	ExpandedRecordHeader *erh;
	uint64		tupdesc_id;
	MemoryContext objcxt;
	MemoryContext oldcxt;
	char	   *chunk;

	if (tupdesc->tdtypeid != RECORDOID)
	{
		/*
		 * If it's a named composite type (not RECORD), we prefer to reference
		 * the typcache's copy of the tupdesc, which is guaranteed to be
		 * refcounted (the given tupdesc might not be).  In any case, we need
		 * to consult the typcache to get the correct tupdesc identifier.
		 *
		 * Note that tdtypeid couldn't be a domain type, so we need not
		 * consider that case here.
		 */
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(tupdesc->tdtypeid, TYPECACHE_TUPDESC);
		if (typentry->tupDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(tupdesc->tdtypeid))));
		tupdesc = typentry->tupDesc;
		tupdesc_id = typentry->tupDesc_identifier;
	}
	else
	{
		/*
		 * For RECORD types, get the appropriate unique identifier (possibly
		 * freshly assigned).
		 */
		tupdesc_id = assign_record_type_identifier(tupdesc->tdtypeid,
												   tupdesc->tdtypmod);
	}

	/*
	 * Allocate private context for expanded object.  We use a regular-size
	 * context, not a small one, to improve the odds that we can fit a tupdesc
	 * into it without needing an extra malloc block.
	 */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded record",
								   ALLOCSET_DEFAULT_SIZES);

	/*
	 * Since we already know the number of fields in the tupdesc, we can
	 * allocate the dvalues/dnulls arrays along with the record header.  This
	 * is useless if we never need those arrays, but it costs almost nothing,
	 * and it will save a palloc cycle if we do need them.
	 */
	erh = (ExpandedRecordHeader *)
		MemoryContextAlloc(objcxt, MAXALIGN(sizeof(ExpandedRecordHeader))
						   + tupdesc->natts * (sizeof(Datum) + sizeof(bool)));

	/* Ensure all header fields are initialized to 0/null */
	memset(erh, 0, sizeof(ExpandedRecordHeader));

	EOH_init_header(&erh->hdr, &ER_methods, objcxt);
	erh->er_magic = ER_MAGIC;

	/* Set up dvalues/dnulls, with no valid contents as yet */
	chunk = (char *) erh + MAXALIGN(sizeof(ExpandedRecordHeader));
	erh->dvalues = (Datum *) chunk;
	erh->dnulls = (bool *) (chunk + tupdesc->natts * sizeof(Datum));
	erh->nfields = tupdesc->natts;

	/* Fill in composite-type identification info */
	erh->er_decltypeid = erh->er_typeid = tupdesc->tdtypeid;
	erh->er_typmod = tupdesc->tdtypmod;
	erh->er_tupdesc_id = tupdesc_id;

	/*
	 * Copy tupdesc if needed, but we prefer to bump its refcount if possible.
	 * We manage the refcount with a memory context callback rather than
	 * assuming that the CurrentResourceOwner is longer-lived than this
	 * expanded object.
	 */
	if (tupdesc->tdrefcount >= 0)
	{
		/* Register callback to release the refcount */
		erh->er_mcb.func = ER_mc_callback;
		erh->er_mcb.arg = erh;
		MemoryContextRegisterResetCallback(erh->hdr.eoh_context,
										   &erh->er_mcb);

		/* And save the pointer */
		erh->er_tupdesc = tupdesc;
		tupdesc->tdrefcount++;
	}
	else
	{
		/* Just copy it */
		oldcxt = MemoryContextSwitchTo(objcxt);
		erh->er_tupdesc = CreateTupleDescCopy(tupdesc);
		erh->flags |= ER_FLAG_TUPDESC_ALLOCED;
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * We don't set ER_FLAG_DVALUES_VALID or ER_FLAG_FVALUE_VALID, so the
	 * record remains logically empty.
	 */

	return erh;
}

/*
 * Build an expanded record of the same rowtype as the given expanded record
 *
 * This is faster than either of the above routines because we can bypass
 * typcache lookup(s).
 *
 * The expanded record is initially "empty" --- we do not copy whatever
 * tuple might be in the source expanded record.
 *
 * The expanded object will be a child of parentcontext.
 */
ExpandedRecordHeader *
make_expanded_record_from_exprecord(ExpandedRecordHeader *olderh,
									MemoryContext parentcontext)
{
	ExpandedRecordHeader *erh;
	TupleDesc	tupdesc = expanded_record_get_tupdesc(olderh);
	MemoryContext objcxt;
	MemoryContext oldcxt;
	char	   *chunk;

	/*
	 * Allocate private context for expanded object.  We use a regular-size
	 * context, not a small one, to improve the odds that we can fit a tupdesc
	 * into it without needing an extra malloc block.
	 */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded record",
								   ALLOCSET_DEFAULT_SIZES);

	/*
	 * Since we already know the number of fields in the tupdesc, we can
	 * allocate the dvalues/dnulls arrays along with the record header.  This
	 * is useless if we never need those arrays, but it costs almost nothing,
	 * and it will save a palloc cycle if we do need them.
	 */
	erh = (ExpandedRecordHeader *)
		MemoryContextAlloc(objcxt, MAXALIGN(sizeof(ExpandedRecordHeader))
						   + tupdesc->natts * (sizeof(Datum) + sizeof(bool)));

	/* Ensure all header fields are initialized to 0/null */
	memset(erh, 0, sizeof(ExpandedRecordHeader));

	EOH_init_header(&erh->hdr, &ER_methods, objcxt);
	erh->er_magic = ER_MAGIC;

	/* Set up dvalues/dnulls, with no valid contents as yet */
	chunk = (char *) erh + MAXALIGN(sizeof(ExpandedRecordHeader));
	erh->dvalues = (Datum *) chunk;
	erh->dnulls = (bool *) (chunk + tupdesc->natts * sizeof(Datum));
	erh->nfields = tupdesc->natts;

	/* Fill in composite-type identification info */
	erh->er_decltypeid = olderh->er_decltypeid;
	erh->er_typeid = olderh->er_typeid;
	erh->er_typmod = olderh->er_typmod;
	erh->er_tupdesc_id = olderh->er_tupdesc_id;

	/* The only flag bit that transfers over is IS_DOMAIN */
	erh->flags = olderh->flags & ER_FLAG_IS_DOMAIN;

	/*
	 * Copy tupdesc if needed, but we prefer to bump its refcount if possible.
	 * We manage the refcount with a memory context callback rather than
	 * assuming that the CurrentResourceOwner is longer-lived than this
	 * expanded object.
	 */
	if (tupdesc->tdrefcount >= 0)
	{
		/* Register callback to release the refcount */
		erh->er_mcb.func = ER_mc_callback;
		erh->er_mcb.arg = erh;
		MemoryContextRegisterResetCallback(erh->hdr.eoh_context,
										   &erh->er_mcb);

		/* And save the pointer */
		erh->er_tupdesc = tupdesc;
		tupdesc->tdrefcount++;
	}
	else if (olderh->flags & ER_FLAG_TUPDESC_ALLOCED)
	{
		/* We need to make our own copy of the tupdesc */
		oldcxt = MemoryContextSwitchTo(objcxt);
		erh->er_tupdesc = CreateTupleDescCopy(tupdesc);
		erh->flags |= ER_FLAG_TUPDESC_ALLOCED;
		MemoryContextSwitchTo(oldcxt);
	}
	else
	{
		/*
		 * Assume the tupdesc will outlive this expanded object, just like
		 * we're assuming it will outlive the source object.
		 */
		erh->er_tupdesc = tupdesc;
	}

	/*
	 * We don't set ER_FLAG_DVALUES_VALID or ER_FLAG_FVALUE_VALID, so the
	 * record remains logically empty.
	 */

	return erh;
}

/*
 * Insert given tuple as the value of the expanded record
 *
 * It is caller's responsibility that the tuple matches the record's
 * previously-assigned rowtype.  (However domain constraints, if any,
 * will be checked here.)
 *
 * The tuple is physically copied into the expanded record's local storage
 * if "copy" is true, otherwise it's caller's responsibility that the tuple
 * will live as long as the expanded record does.
 *
 * Out-of-line field values in the tuple are automatically inlined if
 * "expand_external" is true, otherwise not.  (The combination copy = false,
 * expand_external = true is not sensible and not supported.)
 *
 * Alternatively, tuple can be NULL, in which case we just set the expanded
 * record to be empty.
 */
void
expanded_record_set_tuple(ExpandedRecordHeader *erh,
						  HeapTuple tuple,
						  bool copy,
						  bool expand_external)
{
	int			oldflags;
	HeapTuple	oldtuple;
	char	   *oldfstartptr;
	char	   *oldfendptr;
	int			newflags;
	HeapTuple	newtuple;
	MemoryContext oldcxt;

	/* Shouldn't ever be trying to assign new data to a dummy header */
	Assert(!(erh->flags & ER_FLAG_IS_DUMMY));

	/*
	 * Before performing the assignment, see if result will satisfy domain.
	 */
	if (erh->flags & ER_FLAG_IS_DOMAIN)
		check_domain_for_new_tuple(erh, tuple);

	/*
	 * If we need to get rid of out-of-line field values, do so, using the
	 * short-term context to avoid leaking whatever cruft the toast fetch
	 * might generate.
	 */
	if (expand_external && tuple)
	{
		/* Assert caller didn't ask for unsupported case */
		Assert(copy);
		if (HeapTupleHasExternal(tuple))
		{
			oldcxt = MemoryContextSwitchTo(get_short_term_cxt(erh));
			tuple = toast_flatten_tuple(tuple, erh->er_tupdesc);
			MemoryContextSwitchTo(oldcxt);
		}
		else
			expand_external = false;	/* need not clean up below */
	}

	/*
	 * Initialize new flags, keeping only non-data status bits.
	 */
	oldflags = erh->flags;
	newflags = oldflags & ER_FLAGS_NON_DATA;

	/*
	 * Copy tuple into local storage if needed.  We must be sure this succeeds
	 * before we start to modify the expanded record's state.
	 */
	if (copy && tuple)
	{
		oldcxt = MemoryContextSwitchTo(erh->hdr.eoh_context);
		newtuple = heap_copytuple(tuple);
		newflags |= ER_FLAG_FVALUE_ALLOCED;
		MemoryContextSwitchTo(oldcxt);

		/* We can now flush anything that detoasting might have leaked. */
		if (expand_external)
			MemoryContextReset(erh->er_short_term_cxt);
	}
	else
		newtuple = tuple;

	/* Make copies of fields we're about to overwrite */
	oldtuple = erh->fvalue;
	oldfstartptr = erh->fstartptr;
	oldfendptr = erh->fendptr;

	/*
	 * It's now safe to update the expanded record's state.
	 */
	if (newtuple)
	{
		/* Save flat representation */
		erh->fvalue = newtuple;
		erh->fstartptr = (char *) newtuple->t_data;
		erh->fendptr = ((char *) newtuple->t_data) + newtuple->t_len;
		newflags |= ER_FLAG_FVALUE_VALID;

		/* Remember if we have any out-of-line field values */
		if (HeapTupleHasExternal(newtuple))
			newflags |= ER_FLAG_HAVE_EXTERNAL;
	}
	else
	{
		erh->fvalue = NULL;
		erh->fstartptr = erh->fendptr = NULL;
	}

	erh->flags = newflags;

	/* Reset flat-size info; we don't bother to make it valid now */
	erh->flat_size = 0;

	/*
	 * Now, release any storage belonging to old field values.  It's safe to
	 * do this because ER_FLAG_DVALUES_VALID is no longer set in erh->flags;
	 * even if we fail partway through, the record is valid, and at worst
	 * we've failed to reclaim some space.
	 */
	if (oldflags & ER_FLAG_DVALUES_ALLOCED)
	{
		TupleDesc	tupdesc = erh->er_tupdesc;
		int			i;

		for (i = 0; i < erh->nfields; i++)
		{
			if (!erh->dnulls[i] &&
				!(TupleDescAttr(tupdesc, i)->attbyval))
			{
				char	   *oldValue = (char *) DatumGetPointer(erh->dvalues[i]);

				if (oldValue < oldfstartptr || oldValue >= oldfendptr)
					pfree(oldValue);
			}
		}
	}

	/* Likewise free the old tuple, if it was locally allocated */
	if (oldflags & ER_FLAG_FVALUE_ALLOCED)
		heap_freetuple(oldtuple);

	/* We won't make a new deconstructed representation until/unless needed */
}

/*
 * make_expanded_record_from_datum: build expanded record from composite Datum
 *
 * This combines the functions of make_expanded_record_from_typeid and
 * expanded_record_set_tuple.  However, we do not force a lookup of the
 * tupdesc immediately, reasoning that it might never be needed.
 *
 * The expanded object will be a child of parentcontext.
 *
 * Note: a composite datum cannot self-identify as being of a domain type,
 * so we need not consider domain cases here.
 */
Datum
make_expanded_record_from_datum(Datum recorddatum, MemoryContext parentcontext)
{
	ExpandedRecordHeader *erh;
	HeapTupleHeader tuphdr;
	HeapTupleData tmptup;
	HeapTuple	newtuple;
	MemoryContext objcxt;
	MemoryContext oldcxt;

	/*
	 * Allocate private context for expanded object.  We use a regular-size
	 * context, not a small one, to improve the odds that we can fit a tupdesc
	 * into it without needing an extra malloc block.
	 */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded record",
								   ALLOCSET_DEFAULT_SIZES);

	/* Set up expanded record header, initializing fields to 0/null */
	erh = (ExpandedRecordHeader *)
		MemoryContextAllocZero(objcxt, sizeof(ExpandedRecordHeader));

	EOH_init_header(&erh->hdr, &ER_methods, objcxt);
	erh->er_magic = ER_MAGIC;

	/*
	 * Detoast and copy source record into private context, as a HeapTuple.
	 * (If we actually have to detoast the source, we'll leak some memory in
	 * the caller's context, but it doesn't seem worth worrying about.)
	 */
	tuphdr = DatumGetHeapTupleHeader(recorddatum);

	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuphdr);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tuphdr;

	oldcxt = MemoryContextSwitchTo(objcxt);
	newtuple = heap_copytuple(&tmptup);
	erh->flags |= ER_FLAG_FVALUE_ALLOCED;
	MemoryContextSwitchTo(oldcxt);

	/* Fill in composite-type identification info */
	erh->er_decltypeid = erh->er_typeid = HeapTupleHeaderGetTypeId(tuphdr);
	erh->er_typmod = HeapTupleHeaderGetTypMod(tuphdr);

	/* remember we have a flat representation */
	erh->fvalue = newtuple;
	erh->fstartptr = (char *) newtuple->t_data;
	erh->fendptr = ((char *) newtuple->t_data) + newtuple->t_len;
	erh->flags |= ER_FLAG_FVALUE_VALID;

	/* Shouldn't need to set ER_FLAG_HAVE_EXTERNAL */
	Assert(!HeapTupleHeaderHasExternal(tuphdr));

	/*
	 * We won't look up the tupdesc till we have to, nor make a deconstructed
	 * representation.  We don't have enough info to fill flat_size and
	 * friends, either.
	 */

	/* return a R/W pointer to the expanded record */
	return EOHPGetRWDatum(&erh->hdr);
}

/*
 * get_flat_size method for expanded records
 *
 * Note: call this in a reasonably short-lived memory context, in case of
 * memory leaks from activities such as detoasting.
 */
static Size
ER_get_flat_size(ExpandedObjectHeader *eohptr)
{
	ExpandedRecordHeader *erh = (ExpandedRecordHeader *) eohptr;
	TupleDesc	tupdesc;
	Size		len;
	Size		data_len;
	int			hoff;
	bool		hasnull;
	int			i;

	Assert(erh->er_magic == ER_MAGIC);

	/*
	 * The flat representation has to be a valid composite datum.  Make sure
	 * that we have a registered, not anonymous, RECORD type.
	 */
	if (erh->er_typeid == RECORDOID &&
		erh->er_typmod < 0)
	{
		tupdesc = expanded_record_get_tupdesc(erh);
		assign_record_type_typmod(tupdesc);
		erh->er_typmod = tupdesc->tdtypmod;
	}

	/*
	 * If we have a valid flattened value without out-of-line fields, we can
	 * just use it as-is.
	 */
	if (erh->flags & ER_FLAG_FVALUE_VALID &&
		!(erh->flags & ER_FLAG_HAVE_EXTERNAL))
		return erh->fvalue->t_len;

	/* If we have a cached size value, believe that */
	if (erh->flat_size)
		return erh->flat_size;

	/* If we haven't yet deconstructed the tuple, do that */
	if (!(erh->flags & ER_FLAG_DVALUES_VALID))
		deconstruct_expanded_record(erh);

	/* Tuple descriptor must be valid by now */
	tupdesc = erh->er_tupdesc;

	/*
	 * Composite datums mustn't contain any out-of-line values.
	 */
	if (erh->flags & ER_FLAG_HAVE_EXTERNAL)
	{
		for (i = 0; i < erh->nfields; i++)
		{
			CompactAttribute *attr = TupleDescCompactAttr(tupdesc, i);

			if (!erh->dnulls[i] &&
				!attr->attbyval && attr->attlen == -1 &&
				VARATT_IS_EXTERNAL(DatumGetPointer(erh->dvalues[i])))
			{
				/*
				 * expanded_record_set_field_internal can do the actual work
				 * of detoasting.  It needn't recheck domain constraints.
				 */
				expanded_record_set_field_internal(erh, i + 1,
												   erh->dvalues[i], false,
												   true,
												   false);
			}
		}

		/*
		 * We have now removed all external field values, so we can clear the
		 * flag about them.  This won't cause ER_flatten_into() to mistakenly
		 * take the fast path, since expanded_record_set_field() will have
		 * cleared ER_FLAG_FVALUE_VALID.
		 */
		erh->flags &= ~ER_FLAG_HAVE_EXTERNAL;
	}

	/* Test if we currently have any null values */
	hasnull = false;
	for (i = 0; i < erh->nfields; i++)
	{
		if (erh->dnulls[i])
		{
			hasnull = true;
			break;
		}
	}

	/* Determine total space needed */
	len = offsetof(HeapTupleHeaderData, t_bits);

	if (hasnull)
		len += BITMAPLEN(tupdesc->natts);

	hoff = len = MAXALIGN(len); /* align user data safely */

	data_len = heap_compute_data_size(tupdesc, erh->dvalues, erh->dnulls);

	len += data_len;

	/* Cache for next time */
	erh->flat_size = len;
	erh->data_len = data_len;
	erh->hoff = hoff;
	erh->hasnull = hasnull;

	return len;
}

/*
 * flatten_into method for expanded records
 */
static void
ER_flatten_into(ExpandedObjectHeader *eohptr,
				void *result, Size allocated_size)
{
	ExpandedRecordHeader *erh = (ExpandedRecordHeader *) eohptr;
	HeapTupleHeader tuphdr = (HeapTupleHeader) result;
	TupleDesc	tupdesc;

	Assert(erh->er_magic == ER_MAGIC);

	/* Easy if we have a valid flattened value without out-of-line fields */
	if (erh->flags & ER_FLAG_FVALUE_VALID &&
		!(erh->flags & ER_FLAG_HAVE_EXTERNAL))
	{
		Assert(allocated_size == erh->fvalue->t_len);
		memcpy(tuphdr, erh->fvalue->t_data, allocated_size);
		/* The original flattened value might not have datum header fields */
		HeapTupleHeaderSetDatumLength(tuphdr, allocated_size);
		HeapTupleHeaderSetTypeId(tuphdr, erh->er_typeid);
		HeapTupleHeaderSetTypMod(tuphdr, erh->er_typmod);
		return;
	}

	/* Else allocation should match previous get_flat_size result */
	Assert(allocated_size == erh->flat_size);

	/* We'll need the tuple descriptor */
	tupdesc = expanded_record_get_tupdesc(erh);

	/* We must ensure that any pad space is zero-filled */
	memset(tuphdr, 0, allocated_size);

	/* Set up header fields of composite Datum */
	HeapTupleHeaderSetDatumLength(tuphdr, allocated_size);
	HeapTupleHeaderSetTypeId(tuphdr, erh->er_typeid);
	HeapTupleHeaderSetTypMod(tuphdr, erh->er_typmod);
	/* We also make sure that t_ctid is invalid unless explicitly set */
	ItemPointerSetInvalid(&(tuphdr->t_ctid));

	HeapTupleHeaderSetNatts(tuphdr, tupdesc->natts);
	tuphdr->t_hoff = erh->hoff;

	/* And fill the data area from dvalues/dnulls */
	heap_fill_tuple(tupdesc,
					erh->dvalues,
					erh->dnulls,
					(char *) tuphdr + erh->hoff,
					erh->data_len,
					&tuphdr->t_infomask,
					(erh->hasnull ? tuphdr->t_bits : NULL));
}

/*
 * Look up the tupdesc for the expanded record's actual type
 *
 * Note: code internal to this module is allowed to just fetch
 * erh->er_tupdesc if ER_FLAG_DVALUES_VALID is set; otherwise it should call
 * expanded_record_get_tupdesc.  This function is the out-of-line portion
 * of expanded_record_get_tupdesc.
 */
TupleDesc
expanded_record_fetch_tupdesc(ExpandedRecordHeader *erh)
{
	TupleDesc	tupdesc;

	/* Easy if we already have it (but caller should have checked already) */
	if (erh->er_tupdesc)
		return erh->er_tupdesc;

	/* Lookup the composite type's tupdesc using the typcache */
	tupdesc = lookup_rowtype_tupdesc(erh->er_typeid, erh->er_typmod);

	/*
	 * If it's a refcounted tupdesc rather than a statically allocated one, we
	 * want to manage the refcount with a memory context callback rather than
	 * assuming that the CurrentResourceOwner is longer-lived than this
	 * expanded object.
	 */
	if (tupdesc->tdrefcount >= 0)
	{
		/* Register callback if we didn't already */
		if (erh->er_mcb.arg == NULL)
		{
			erh->er_mcb.func = ER_mc_callback;
			erh->er_mcb.arg = erh;
			MemoryContextRegisterResetCallback(erh->hdr.eoh_context,
											   &erh->er_mcb);
		}

		/* Remember our own pointer */
		erh->er_tupdesc = tupdesc;
		tupdesc->tdrefcount++;

		/* Release the pin lookup_rowtype_tupdesc acquired */
		ReleaseTupleDesc(tupdesc);
	}
	else
	{
		/* Just remember the pointer */
		erh->er_tupdesc = tupdesc;
	}

	/* In either case, fetch the process-global ID for this tupdesc */
	erh->er_tupdesc_id = assign_record_type_identifier(tupdesc->tdtypeid,
													   tupdesc->tdtypmod);

	return tupdesc;
}

/*
 * Get a HeapTuple representing the current value of the expanded record
 *
 * If valid, the originally stored tuple is returned, so caller must not
 * scribble on it.  Otherwise, we return a HeapTuple created in the current
 * memory context.  In either case, no attempt has been made to inline
 * out-of-line toasted values, so the tuple isn't usable as a composite
 * datum.
 *
 * Returns NULL if expanded record is empty.
 */
HeapTuple
expanded_record_get_tuple(ExpandedRecordHeader *erh)
{
	/* Easy case if we still have original tuple */
	if (erh->flags & ER_FLAG_FVALUE_VALID)
		return erh->fvalue;

	/* Else just build a tuple from datums */
	if (erh->flags & ER_FLAG_DVALUES_VALID)
		return heap_form_tuple(erh->er_tupdesc, erh->dvalues, erh->dnulls);

	/* Expanded record is empty */
	return NULL;
}

/*
 * Memory context reset callback for cleaning up external resources
 */
static void
ER_mc_callback(void *arg)
{
	ExpandedRecordHeader *erh = (ExpandedRecordHeader *) arg;
	TupleDesc	tupdesc = erh->er_tupdesc;

	/* Release our privately-managed tupdesc refcount, if any */
	if (tupdesc)
	{
		erh->er_tupdesc = NULL; /* just for luck */
		if (tupdesc->tdrefcount > 0)
		{
			if (--tupdesc->tdrefcount == 0)
				FreeTupleDesc(tupdesc);
		}
	}
}

/*
 * DatumGetExpandedRecord: get a writable expanded record from an input argument
 *
 * Caution: if the input is a read/write pointer, this returns the input
 * argument; so callers must be sure that their changes are "safe", that is
 * they cannot leave the record in a corrupt state.
 */
ExpandedRecordHeader *
DatumGetExpandedRecord(Datum d)
{
	/* If it's a writable expanded record already, just return it */
	if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)))
	{
		ExpandedRecordHeader *erh = (ExpandedRecordHeader *) DatumGetEOHP(d);

		Assert(erh->er_magic == ER_MAGIC);
		return erh;
	}

	/* Else expand the hard way */
	d = make_expanded_record_from_datum(d, CurrentMemoryContext);
	return (ExpandedRecordHeader *) DatumGetEOHP(d);
}

/*
 * Create the Datum/isnull representation of an expanded record object
 * if we didn't do so already.  After calling this, it's OK to read the
 * dvalues/dnulls arrays directly, rather than going through get_field.
 *
 * Note that if the object is currently empty ("null"), this will change
 * it to represent a row of nulls.
 */
void
deconstruct_expanded_record(ExpandedRecordHeader *erh)
{
	TupleDesc	tupdesc;
	Datum	   *dvalues;
	bool	   *dnulls;
	int			nfields;

	if (erh->flags & ER_FLAG_DVALUES_VALID)
		return;					/* already valid, nothing to do */

	/* We'll need the tuple descriptor */
	tupdesc = expanded_record_get_tupdesc(erh);

	/*
	 * Allocate arrays in private context, if we don't have them already.  We
	 * don't expect to see a change in nfields here, so while we cope if it
	 * happens, we don't bother avoiding a leak of the old arrays (which might
	 * not be separately palloc'd, anyway).
	 */
	nfields = tupdesc->natts;
	if (erh->dvalues == NULL || erh->nfields != nfields)
	{
		char	   *chunk;

		/*
		 * To save a palloc cycle, we allocate both the Datum and isnull
		 * arrays in one palloc chunk.
		 */
		chunk = MemoryContextAlloc(erh->hdr.eoh_context,
								   nfields * (sizeof(Datum) + sizeof(bool)));
		dvalues = (Datum *) chunk;
		dnulls = (bool *) (chunk + nfields * sizeof(Datum));
		erh->dvalues = dvalues;
		erh->dnulls = dnulls;
		erh->nfields = nfields;
	}
	else
	{
		dvalues = erh->dvalues;
		dnulls = erh->dnulls;
	}

	if (erh->flags & ER_FLAG_FVALUE_VALID)
	{
		/* Deconstruct tuple */
		heap_deform_tuple(erh->fvalue, tupdesc, dvalues, dnulls);
	}
	else
	{
		/* If record was empty, instantiate it as a row of nulls */
		memset(dvalues, 0, nfields * sizeof(Datum));
		memset(dnulls, true, nfields * sizeof(bool));
	}

	/* Mark the dvalues as valid */
	erh->flags |= ER_FLAG_DVALUES_VALID;
}

/*
 * Look up a record field by name
 *
 * If there is a field named "fieldname", fill in the contents of finfo
 * and return "true".  Else return "false" without changing *finfo.
 */
bool
expanded_record_lookup_field(ExpandedRecordHeader *erh, const char *fieldname,
							 ExpandedRecordFieldInfo *finfo)
{
	TupleDesc	tupdesc;
	int			fno;
	Form_pg_attribute attr;
	const FormData_pg_attribute *sysattr;

	tupdesc = expanded_record_get_tupdesc(erh);

	/* First, check user-defined attributes */
	for (fno = 0; fno < tupdesc->natts; fno++)
	{
		attr = TupleDescAttr(tupdesc, fno);
		if (namestrcmp(&attr->attname, fieldname) == 0 &&
			!attr->attisdropped)
		{
			finfo->fnumber = attr->attnum;
			finfo->ftypeid = attr->atttypid;
			finfo->ftypmod = attr->atttypmod;
			finfo->fcollation = attr->attcollation;
			return true;
		}
	}

	/* How about system attributes? */
	sysattr = SystemAttributeByName(fieldname);
	if (sysattr != NULL)
	{
		finfo->fnumber = sysattr->attnum;
		finfo->ftypeid = sysattr->atttypid;
		finfo->ftypmod = sysattr->atttypmod;
		finfo->fcollation = sysattr->attcollation;
		return true;
	}

	return false;
}

/*
 * Fetch value of record field
 *
 * expanded_record_get_field is the frontend for this; it handles the
 * easy inline-able cases.
 */
Datum
expanded_record_fetch_field(ExpandedRecordHeader *erh, int fnumber,
							bool *isnull)
{
	if (fnumber > 0)
	{
		/* Empty record has null fields */
		if (ExpandedRecordIsEmpty(erh))
		{
			*isnull = true;
			return (Datum) 0;
		}
		/* Make sure we have deconstructed form */
		deconstruct_expanded_record(erh);
		/* Out-of-range field number reads as null */
		if (unlikely(fnumber > erh->nfields))
		{
			*isnull = true;
			return (Datum) 0;
		}
		*isnull = erh->dnulls[fnumber - 1];
		return erh->dvalues[fnumber - 1];
	}
	else
	{
		/* System columns read as null if we haven't got flat tuple */
		if (erh->fvalue == NULL)
		{
			*isnull = true;
			return (Datum) 0;
		}
		/* heap_getsysattr doesn't actually use tupdesc, so just pass null */
		return heap_getsysattr(erh->fvalue, fnumber, NULL, isnull);
	}
}

/*
 * Set value of record field
 *
 * If the expanded record is of domain type, the assignment will be rejected
 * (without changing the record's state) if the domain's constraints would
 * be violated.
 *
 * If expand_external is true and newValue is an out-of-line value, we'll
 * forcibly detoast it so that the record does not depend on external storage.
 *
 * Internal callers can pass check_constraints = false to skip application
 * of domain constraints.  External callers should never do that.
 */
void
expanded_record_set_field_internal(ExpandedRecordHeader *erh, int fnumber,
								   Datum newValue, bool isnull,
								   bool expand_external,
								   bool check_constraints)
{
	TupleDesc	tupdesc;
	CompactAttribute *attr;
	Datum	   *dvalues;
	bool	   *dnulls;
	char	   *oldValue;

	/*
	 * Shouldn't ever be trying to assign new data to a dummy header, except
	 * in the case of an internal call for field inlining.
	 */
	Assert(!(erh->flags & ER_FLAG_IS_DUMMY) || !check_constraints);

	/* Before performing the assignment, see if result will satisfy domain */
	if ((erh->flags & ER_FLAG_IS_DOMAIN) && check_constraints)
		check_domain_for_new_field(erh, fnumber, newValue, isnull);

	/* If we haven't yet deconstructed the tuple, do that */
	if (!(erh->flags & ER_FLAG_DVALUES_VALID))
		deconstruct_expanded_record(erh);

	/* Tuple descriptor must be valid by now */
	tupdesc = erh->er_tupdesc;
	Assert(erh->nfields == tupdesc->natts);

	/* Caller error if fnumber is system column or nonexistent column */
	if (unlikely(fnumber <= 0 || fnumber > erh->nfields))
		elog(ERROR, "cannot assign to field %d of expanded record", fnumber);

	/*
	 * Copy new field value into record's context, and deal with detoasting,
	 * if needed.
	 */
	attr = TupleDescCompactAttr(tupdesc, fnumber - 1);
	if (!isnull && !attr->attbyval)
	{
		MemoryContext oldcxt;

		/* If requested, detoast any external value */
		if (expand_external)
		{
			if (attr->attlen == -1 &&
				VARATT_IS_EXTERNAL(DatumGetPointer(newValue)))
			{
				/* Detoasting should be done in short-lived context. */
				oldcxt = MemoryContextSwitchTo(get_short_term_cxt(erh));
				newValue = PointerGetDatum(detoast_external_attr((struct varlena *) DatumGetPointer(newValue)));
				MemoryContextSwitchTo(oldcxt);
			}
			else
				expand_external = false;	/* need not clean up below */
		}

		/* Copy value into record's context */
		oldcxt = MemoryContextSwitchTo(erh->hdr.eoh_context);
		newValue = datumCopy(newValue, false, attr->attlen);
		MemoryContextSwitchTo(oldcxt);

		/* We can now flush anything that detoasting might have leaked */
		if (expand_external)
			MemoryContextReset(erh->er_short_term_cxt);

		/* Remember that we have field(s) that may need to be pfree'd */
		erh->flags |= ER_FLAG_DVALUES_ALLOCED;

		/*
		 * While we're here, note whether it's an external toasted value,
		 * because that could mean we need to inline it later.  (Think not to
		 * merge this into the previous expand_external logic: datumCopy could
		 * by itself have made the value non-external.)
		 */
		if (attr->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(newValue)))
			erh->flags |= ER_FLAG_HAVE_EXTERNAL;
	}

	/*
	 * We're ready to make irreversible changes.
	 */
	dvalues = erh->dvalues;
	dnulls = erh->dnulls;

	/* Flattened value will no longer represent record accurately */
	erh->flags &= ~ER_FLAG_FVALUE_VALID;
	/* And we don't know the flattened size either */
	erh->flat_size = 0;

	/* Grab old field value for pfree'ing, if needed. */
	if (!attr->attbyval && !dnulls[fnumber - 1])
		oldValue = (char *) DatumGetPointer(dvalues[fnumber - 1]);
	else
		oldValue = NULL;

	/* And finally we can insert the new field. */
	dvalues[fnumber - 1] = newValue;
	dnulls[fnumber - 1] = isnull;

	/*
	 * Free old field if needed; this keeps repeated field replacements from
	 * bloating the record's storage.  If the pfree somehow fails, it won't
	 * corrupt the record.
	 *
	 * If we're updating a dummy header, we can't risk pfree'ing the old
	 * value, because most likely the expanded record's main header still has
	 * a pointer to it.  This won't result in any sustained memory leak, since
	 * whatever we just allocated here is in the short-lived domain check
	 * context.
	 */
	if (oldValue && !(erh->flags & ER_FLAG_IS_DUMMY))
	{
		/* Don't try to pfree a part of the original flat record */
		if (oldValue < erh->fstartptr || oldValue >= erh->fendptr)
			pfree(oldValue);
	}
}

/*
 * Set all record field(s)
 *
 * Caller must ensure that the provided datums are of the right types
 * to match the record's previously assigned rowtype.
 *
 * If expand_external is true, we'll forcibly detoast out-of-line field values
 * so that the record does not depend on external storage.
 *
 * Unlike repeated application of expanded_record_set_field(), this does not
 * guarantee to leave the expanded record in a non-corrupt state in event
 * of an error.  Typically it would only be used for initializing a new
 * expanded record.  Also, because we expect this to be applied at most once
 * in the lifespan of an expanded record, we do not worry about any cruft
 * that detoasting might leak.
 */
void
expanded_record_set_fields(ExpandedRecordHeader *erh,
						   const Datum *newValues, const bool *isnulls,
						   bool expand_external)
{
	TupleDesc	tupdesc;
	Datum	   *dvalues;
	bool	   *dnulls;
	int			fnumber;
	MemoryContext oldcxt;

	/* Shouldn't ever be trying to assign new data to a dummy header */
	Assert(!(erh->flags & ER_FLAG_IS_DUMMY));

	/* If we haven't yet deconstructed the tuple, do that */
	if (!(erh->flags & ER_FLAG_DVALUES_VALID))
		deconstruct_expanded_record(erh);

	/* Tuple descriptor must be valid by now */
	tupdesc = erh->er_tupdesc;
	Assert(erh->nfields == tupdesc->natts);

	/* Flattened value will no longer represent record accurately */
	erh->flags &= ~ER_FLAG_FVALUE_VALID;
	/* And we don't know the flattened size either */
	erh->flat_size = 0;

	oldcxt = MemoryContextSwitchTo(erh->hdr.eoh_context);

	dvalues = erh->dvalues;
	dnulls = erh->dnulls;

	for (fnumber = 0; fnumber < erh->nfields; fnumber++)
	{
		CompactAttribute *attr = TupleDescCompactAttr(tupdesc, fnumber);
		Datum		newValue;
		bool		isnull;

		/* Ignore dropped columns */
		if (attr->attisdropped)
			continue;

		newValue = newValues[fnumber];
		isnull = isnulls[fnumber];

		if (!attr->attbyval)
		{
			/*
			 * Copy new field value into record's context, and deal with
			 * detoasting, if needed.
			 */
			if (!isnull)
			{
				/* Is it an external toasted value? */
				if (attr->attlen == -1 &&
					VARATT_IS_EXTERNAL(DatumGetPointer(newValue)))
				{
					if (expand_external)
					{
						/* Detoast as requested while copying the value */
						newValue = PointerGetDatum(detoast_external_attr((struct varlena *) DatumGetPointer(newValue)));
					}
					else
					{
						/* Just copy the value */
						newValue = datumCopy(newValue, false, -1);
						/* If it's still external, remember that */
						if (VARATT_IS_EXTERNAL(DatumGetPointer(newValue)))
							erh->flags |= ER_FLAG_HAVE_EXTERNAL;
					}
				}
				else
				{
					/* Not an external value, just copy it */
					newValue = datumCopy(newValue, false, attr->attlen);
				}

				/* Remember that we have field(s) that need to be pfree'd */
				erh->flags |= ER_FLAG_DVALUES_ALLOCED;
			}

			/*
			 * Free old field value, if any (not likely, since really we ought
			 * to be inserting into an empty record).
			 */
			if (unlikely(!dnulls[fnumber]))
			{
				char	   *oldValue;

				oldValue = (char *) DatumGetPointer(dvalues[fnumber]);
				/* Don't try to pfree a part of the original flat record */
				if (oldValue < erh->fstartptr || oldValue >= erh->fendptr)
					pfree(oldValue);
			}
		}

		/* And finally we can insert the new field. */
		dvalues[fnumber] = newValue;
		dnulls[fnumber] = isnull;
	}

	/*
	 * Because we don't guarantee atomicity of set_fields(), we can just leave
	 * checking of domain constraints to occur as the final step; if it throws
	 * an error, too bad.
	 */
	if (erh->flags & ER_FLAG_IS_DOMAIN)
	{
		/* We run domain_check in a short-lived context to limit cruft */
		MemoryContextSwitchTo(get_short_term_cxt(erh));

		domain_check(ExpandedRecordGetRODatum(erh), false,
					 erh->er_decltypeid,
					 &erh->er_domaininfo,
					 erh->hdr.eoh_context);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Construct (or reset) working memory context for short-term operations.
 *
 * This context is used for domain check evaluation and for detoasting.
 *
 * If we don't have a short-lived memory context, make one; if we have one,
 * reset it to get rid of any leftover cruft.  (It is a tad annoying to need a
 * whole context for this, since it will often go unused --- but it's hard to
 * avoid memory leaks otherwise.  We can make the context small, at least.)
 */
static MemoryContext
get_short_term_cxt(ExpandedRecordHeader *erh)
{
	if (erh->er_short_term_cxt == NULL)
		erh->er_short_term_cxt =
			AllocSetContextCreate(erh->hdr.eoh_context,
								  "expanded record short-term context",
								  ALLOCSET_SMALL_SIZES);
	else
		MemoryContextReset(erh->er_short_term_cxt);
	return erh->er_short_term_cxt;
}

/*
 * Construct "dummy header" for checking domain constraints.
 *
 * Since we don't want to modify the state of the expanded record until
 * we've validated the constraints, our approach is to set up a dummy
 * record header containing the new field value(s) and then pass that to
 * domain_check.  We retain the dummy header as part of the expanded
 * record's state to save palloc cycles, but reinitialize (most of)
 * its contents on each use.
 */
static void
build_dummy_expanded_header(ExpandedRecordHeader *main_erh)
{
	ExpandedRecordHeader *erh;
	TupleDesc	tupdesc = expanded_record_get_tupdesc(main_erh);

	/* Ensure we have a short-lived context */
	(void) get_short_term_cxt(main_erh);

	/*
	 * Allocate dummy header on first time through, or in the unlikely event
	 * that the number of fields changes (in which case we just leak the old
	 * one).  Include space for its field values in the request.
	 */
	erh = main_erh->er_dummy_header;
	if (erh == NULL || erh->nfields != tupdesc->natts)
	{
		char	   *chunk;

		erh = (ExpandedRecordHeader *)
			MemoryContextAlloc(main_erh->hdr.eoh_context,
							   MAXALIGN(sizeof(ExpandedRecordHeader))
							   + tupdesc->natts * (sizeof(Datum) + sizeof(bool)));

		/* Ensure all header fields are initialized to 0/null */
		memset(erh, 0, sizeof(ExpandedRecordHeader));

		/*
		 * We set up the dummy header with an indication that its memory
		 * context is the short-lived context.  This is so that, if any
		 * detoasting of out-of-line values happens due to an attempt to
		 * extract a composite datum from the dummy header, the detoasted
		 * stuff will end up in the short-lived context and not cause a leak.
		 * This is cheating a bit on the expanded-object protocol; but since
		 * we never pass a R/W pointer to the dummy object to any other code,
		 * nothing else is authorized to delete or transfer ownership of the
		 * object's context, so it should be safe enough.
		 */
		EOH_init_header(&erh->hdr, &ER_methods, main_erh->er_short_term_cxt);
		erh->er_magic = ER_MAGIC;

		/* Set up dvalues/dnulls, with no valid contents as yet */
		chunk = (char *) erh + MAXALIGN(sizeof(ExpandedRecordHeader));
		erh->dvalues = (Datum *) chunk;
		erh->dnulls = (bool *) (chunk + tupdesc->natts * sizeof(Datum));
		erh->nfields = tupdesc->natts;

		/*
		 * The fields we just set are assumed to remain constant through
		 * multiple uses of the dummy header to check domain constraints.  All
		 * other dummy header fields should be explicitly reset below, to
		 * ensure there's not accidental effects of one check on the next one.
		 */

		main_erh->er_dummy_header = erh;
	}

	/*
	 * If anything inquires about the dummy header's declared type, it should
	 * report the composite base type, not the domain type (since the VALUE in
	 * a domain check constraint is of the base type not the domain).  Hence
	 * we do not transfer over the IS_DOMAIN flag, nor indeed any of the main
	 * header's flags, since the dummy header is empty of data at this point.
	 * But don't forget to mark header as dummy.
	 */
	erh->flags = ER_FLAG_IS_DUMMY;

	/* Copy composite-type identification info */
	erh->er_decltypeid = erh->er_typeid = main_erh->er_typeid;
	erh->er_typmod = main_erh->er_typmod;

	/* Dummy header does not need its own tupdesc refcount */
	erh->er_tupdesc = tupdesc;
	erh->er_tupdesc_id = main_erh->er_tupdesc_id;

	/*
	 * It's tempting to copy over whatever we know about the flat size, but
	 * there's no point since we're surely about to modify the dummy record's
	 * field(s).  Instead just clear anything left over from a previous usage
	 * cycle.
	 */
	erh->flat_size = 0;

	/* Copy over fvalue if we have it, so that system columns are available */
	erh->fvalue = main_erh->fvalue;
	erh->fstartptr = main_erh->fstartptr;
	erh->fendptr = main_erh->fendptr;
}

/*
 * Precheck domain constraints for a set_field operation
 */
static pg_noinline void
check_domain_for_new_field(ExpandedRecordHeader *erh, int fnumber,
						   Datum newValue, bool isnull)
{
	ExpandedRecordHeader *dummy_erh;
	MemoryContext oldcxt;

	/* Construct dummy header to contain proposed new field set */
	build_dummy_expanded_header(erh);
	dummy_erh = erh->er_dummy_header;

	/*
	 * If record isn't empty, just deconstruct it (if needed) and copy over
	 * the existing field values.  If it is empty, just fill fields with nulls
	 * manually --- don't call deconstruct_expanded_record prematurely.
	 */
	if (!ExpandedRecordIsEmpty(erh))
	{
		deconstruct_expanded_record(erh);
		memcpy(dummy_erh->dvalues, erh->dvalues,
			   dummy_erh->nfields * sizeof(Datum));
		memcpy(dummy_erh->dnulls, erh->dnulls,
			   dummy_erh->nfields * sizeof(bool));
		/* There might be some external values in there... */
		dummy_erh->flags |= erh->flags & ER_FLAG_HAVE_EXTERNAL;
	}
	else
	{
		memset(dummy_erh->dvalues, 0, dummy_erh->nfields * sizeof(Datum));
		memset(dummy_erh->dnulls, true, dummy_erh->nfields * sizeof(bool));
	}

	/* Either way, we now have valid dvalues */
	dummy_erh->flags |= ER_FLAG_DVALUES_VALID;

	/* Caller error if fnumber is system column or nonexistent column */
	if (unlikely(fnumber <= 0 || fnumber > dummy_erh->nfields))
		elog(ERROR, "cannot assign to field %d of expanded record", fnumber);

	/* Insert proposed new value into dummy field array */
	dummy_erh->dvalues[fnumber - 1] = newValue;
	dummy_erh->dnulls[fnumber - 1] = isnull;

	/*
	 * The proposed new value might be external, in which case we'd better set
	 * the flag for that in dummy_erh.  (This matters in case something in the
	 * domain check expressions tries to extract a flat value from the dummy
	 * header.)
	 */
	if (!isnull)
	{
		CompactAttribute *attr = TupleDescCompactAttr(erh->er_tupdesc, fnumber - 1);

		if (!attr->attbyval && attr->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(newValue)))
			dummy_erh->flags |= ER_FLAG_HAVE_EXTERNAL;
	}

	/*
	 * We call domain_check in the short-lived context, so that any cruft
	 * leaked by expression evaluation can be reclaimed.
	 */
	oldcxt = MemoryContextSwitchTo(erh->er_short_term_cxt);

	/*
	 * And now we can apply the check.  Note we use main header's domain cache
	 * space, so that caching carries across repeated uses.
	 */
	domain_check(ExpandedRecordGetRODatum(dummy_erh), false,
				 erh->er_decltypeid,
				 &erh->er_domaininfo,
				 erh->hdr.eoh_context);

	MemoryContextSwitchTo(oldcxt);

	/* We might as well clean up cruft immediately. */
	MemoryContextReset(erh->er_short_term_cxt);
}

/*
 * Precheck domain constraints for a set_tuple operation
 */
static pg_noinline void
check_domain_for_new_tuple(ExpandedRecordHeader *erh, HeapTuple tuple)
{
	ExpandedRecordHeader *dummy_erh;
	MemoryContext oldcxt;

	/* If we're being told to set record to empty, just see if NULL is OK */
	if (tuple == NULL)
	{
		/* We run domain_check in a short-lived context to limit cruft */
		oldcxt = MemoryContextSwitchTo(get_short_term_cxt(erh));

		domain_check((Datum) 0, true,
					 erh->er_decltypeid,
					 &erh->er_domaininfo,
					 erh->hdr.eoh_context);

		MemoryContextSwitchTo(oldcxt);

		/* We might as well clean up cruft immediately. */
		MemoryContextReset(erh->er_short_term_cxt);

		return;
	}

	/* Construct dummy header to contain replacement tuple */
	build_dummy_expanded_header(erh);
	dummy_erh = erh->er_dummy_header;

	/* Insert tuple, but don't bother to deconstruct its fields for now */
	dummy_erh->fvalue = tuple;
	dummy_erh->fstartptr = (char *) tuple->t_data;
	dummy_erh->fendptr = ((char *) tuple->t_data) + tuple->t_len;
	dummy_erh->flags |= ER_FLAG_FVALUE_VALID;

	/* Remember if we have any out-of-line field values */
	if (HeapTupleHasExternal(tuple))
		dummy_erh->flags |= ER_FLAG_HAVE_EXTERNAL;

	/*
	 * We call domain_check in the short-lived context, so that any cruft
	 * leaked by expression evaluation can be reclaimed.
	 */
	oldcxt = MemoryContextSwitchTo(erh->er_short_term_cxt);

	/*
	 * And now we can apply the check.  Note we use main header's domain cache
	 * space, so that caching carries across repeated uses.
	 */
	domain_check(ExpandedRecordGetRODatum(dummy_erh), false,
				 erh->er_decltypeid,
				 &erh->er_domaininfo,
				 erh->hdr.eoh_context);

	MemoryContextSwitchTo(oldcxt);

	/* We might as well clean up cruft immediately. */
	MemoryContextReset(erh->er_short_term_cxt);
}
