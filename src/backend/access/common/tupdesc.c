/*-------------------------------------------------------------------------
 *
 * tupdesc.c
 *	  POSTGRES tuple descriptor support code
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/tupdesc.c
 *
 * NOTES
 *	  some of the executor utility code such as "ExecTypeFromTL" should be
 *	  moved here.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/toast_compression.h"
#include "access/tupdesc_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/resowner.h"
#include "utils/syscache.h"

/* ResourceOwner callbacks to hold tupledesc references  */
static void ResOwnerReleaseTupleDesc(Datum res);
static char *ResOwnerPrintTupleDesc(Datum res);

static const ResourceOwnerDesc tupdesc_resowner_desc =
{
	.name = "tupdesc reference",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_TUPDESC_REFS,
	.ReleaseResource = ResOwnerReleaseTupleDesc,
	.DebugPrint = ResOwnerPrintTupleDesc
};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void
ResourceOwnerRememberTupleDesc(ResourceOwner owner, TupleDesc tupdesc)
{
	ResourceOwnerRemember(owner, PointerGetDatum(tupdesc), &tupdesc_resowner_desc);
}

static inline void
ResourceOwnerForgetTupleDesc(ResourceOwner owner, TupleDesc tupdesc)
{
	ResourceOwnerForget(owner, PointerGetDatum(tupdesc), &tupdesc_resowner_desc);
}

/*
 * populate_compact_attribute_internal
 *		Helper function for populate_compact_attribute()
 */
static inline void
populate_compact_attribute_internal(Form_pg_attribute src,
									CompactAttribute *dst)
{
	memset(dst, 0, sizeof(CompactAttribute));

	dst->attcacheoff = -1;
	dst->attlen = src->attlen;

	dst->attbyval = src->attbyval;
	dst->attispackable = (src->attstorage != TYPSTORAGE_PLAIN);
	dst->atthasmissing = src->atthasmissing;
	dst->attisdropped = src->attisdropped;
	dst->attgenerated = (src->attgenerated != '\0');
	dst->attnotnull = src->attnotnull;

	switch (src->attalign)
	{
		case TYPALIGN_INT:
			dst->attalignby = ALIGNOF_INT;
			break;
		case TYPALIGN_CHAR:
			dst->attalignby = sizeof(char);
			break;
		case TYPALIGN_DOUBLE:
			dst->attalignby = ALIGNOF_DOUBLE;
			break;
		case TYPALIGN_SHORT:
			dst->attalignby = ALIGNOF_SHORT;
			break;
		default:
			dst->attalignby = 0;
			elog(ERROR, "invalid attalign value: %c", src->attalign);
			break;
	}
}

/*
 * populate_compact_attribute
 *		Fill in the corresponding CompactAttribute element from the
 *		Form_pg_attribute for the given attribute number.  This must be called
 *		whenever a change is made to a Form_pg_attribute in the TupleDesc.
 */
void
populate_compact_attribute(TupleDesc tupdesc, int attnum)
{
	Form_pg_attribute src = TupleDescAttr(tupdesc, attnum);
	CompactAttribute *dst;

	/*
	 * Don't use TupleDescCompactAttr to prevent infinite recursion in assert
	 * builds.
	 */
	dst = &tupdesc->compact_attrs[attnum];

	populate_compact_attribute_internal(src, dst);
}

/*
 * verify_compact_attribute
 *		In Assert enabled builds, we verify that the CompactAttribute is
 *		populated correctly.  This helps find bugs in places such as ALTER
 *		TABLE where code makes changes to the FormData_pg_attribute but
 *		forgets to call populate_compact_attribute().
 *
 * This is used in TupleDescCompactAttr(), but declared here to allow access
 * to populate_compact_attribute_internal().
 */
void
verify_compact_attribute(TupleDesc tupdesc, int attnum)
{
#ifdef USE_ASSERT_CHECKING
	CompactAttribute *cattr = &tupdesc->compact_attrs[attnum];
	Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum);
	CompactAttribute tmp;

	/*
	 * Populate the temporary CompactAttribute from the corresponding
	 * Form_pg_attribute
	 */
	populate_compact_attribute_internal(attr, &tmp);

	/*
	 * Make the attcacheoff match since it's been reset to -1 by
	 * populate_compact_attribute_internal.
	 */
	tmp.attcacheoff = cattr->attcacheoff;

	/* Check the freshly populated CompactAttribute matches the TupleDesc's */
	Assert(memcmp(&tmp, cattr, sizeof(CompactAttribute)) == 0);
#endif
}

/*
 * CreateTemplateTupleDesc
 *		This function allocates an empty tuple descriptor structure.
 *
 * Tuple type ID information is initially set for an anonymous record type;
 * caller can overwrite this if needed.
 */
TupleDesc
CreateTemplateTupleDesc(int natts)
{
	TupleDesc	desc;

	/*
	 * sanity checks
	 */
	Assert(natts >= 0);

	/*
	 * Allocate enough memory for the tuple descriptor, the CompactAttribute
	 * array and also an array of FormData_pg_attribute.
	 *
	 * Note: the FormData_pg_attribute array stride is
	 * sizeof(FormData_pg_attribute), since we declare the array elements as
	 * FormData_pg_attribute for notational convenience.  However, we only
	 * guarantee that the first ATTRIBUTE_FIXED_PART_SIZE bytes of each entry
	 * are valid; most code that copies tupdesc entries around copies just
	 * that much.  In principle that could be less due to trailing padding,
	 * although with the current definition of pg_attribute there probably
	 * isn't any padding.
	 */
	desc = (TupleDesc) palloc(offsetof(struct TupleDescData, compact_attrs) +
							  natts * sizeof(CompactAttribute) +
							  natts * sizeof(FormData_pg_attribute));

	/*
	 * Initialize other fields of the tupdesc.
	 */
	desc->natts = natts;
	desc->constr = NULL;
	desc->tdtypeid = RECORDOID;
	desc->tdtypmod = -1;
	desc->tdrefcount = -1;		/* assume not reference-counted */

	return desc;
}

/*
 * CreateTupleDesc
 *		This function allocates a new TupleDesc by copying a given
 *		Form_pg_attribute array.
 *
 * Tuple type ID information is initially set for an anonymous record type;
 * caller can overwrite this if needed.
 */
TupleDesc
CreateTupleDesc(int natts, Form_pg_attribute *attrs)
{
	TupleDesc	desc;
	int			i;

	desc = CreateTemplateTupleDesc(natts);

	for (i = 0; i < natts; ++i)
	{
		memcpy(TupleDescAttr(desc, i), attrs[i], ATTRIBUTE_FIXED_PART_SIZE);
		populate_compact_attribute(desc, i);
	}
	return desc;
}

/*
 * CreateTupleDescCopy
 *		This function creates a new TupleDesc by copying from an existing
 *		TupleDesc.
 *
 * !!! Constraints and defaults are not copied !!!
 */
TupleDesc
CreateTupleDescCopy(TupleDesc tupdesc)
{
	TupleDesc	desc;
	int			i;

	desc = CreateTemplateTupleDesc(tupdesc->natts);

	/* Flat-copy the attribute array */
	memcpy(TupleDescAttr(desc, 0),
		   TupleDescAttr(tupdesc, 0),
		   desc->natts * sizeof(FormData_pg_attribute));

	/*
	 * Since we're not copying constraints and defaults, clear fields
	 * associated with them.
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		att->attnotnull = false;
		att->atthasdef = false;
		att->atthasmissing = false;
		att->attidentity = '\0';
		att->attgenerated = '\0';

		populate_compact_attribute(desc, i);
	}

	/* We can copy the tuple type identification, too */
	desc->tdtypeid = tupdesc->tdtypeid;
	desc->tdtypmod = tupdesc->tdtypmod;

	return desc;
}

/*
 * CreateTupleDescTruncatedCopy
 *		This function creates a new TupleDesc with only the first 'natts'
 *		attributes from an existing TupleDesc
 *
 * !!! Constraints and defaults are not copied !!!
 */
TupleDesc
CreateTupleDescTruncatedCopy(TupleDesc tupdesc, int natts)
{
	TupleDesc	desc;
	int			i;

	Assert(natts <= tupdesc->natts);

	desc = CreateTemplateTupleDesc(natts);

	/* Flat-copy the attribute array */
	memcpy(TupleDescAttr(desc, 0),
		   TupleDescAttr(tupdesc, 0),
		   desc->natts * sizeof(FormData_pg_attribute));

	/*
	 * Since we're not copying constraints and defaults, clear fields
	 * associated with them.
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		att->attnotnull = false;
		att->atthasdef = false;
		att->atthasmissing = false;
		att->attidentity = '\0';
		att->attgenerated = '\0';

		populate_compact_attribute(desc, i);
	}

	/* We can copy the tuple type identification, too */
	desc->tdtypeid = tupdesc->tdtypeid;
	desc->tdtypmod = tupdesc->tdtypmod;

	return desc;
}

/*
 * CreateTupleDescCopyConstr
 *		This function creates a new TupleDesc by copying from an existing
 *		TupleDesc (including its constraints and defaults).
 */
TupleDesc
CreateTupleDescCopyConstr(TupleDesc tupdesc)
{
	TupleDesc	desc;
	TupleConstr *constr = tupdesc->constr;
	int			i;

	desc = CreateTemplateTupleDesc(tupdesc->natts);

	/* Flat-copy the attribute array */
	memcpy(TupleDescAttr(desc, 0),
		   TupleDescAttr(tupdesc, 0),
		   desc->natts * sizeof(FormData_pg_attribute));

	for (i = 0; i < desc->natts; i++)
		populate_compact_attribute(desc, i);

	/* Copy the TupleConstr data structure, if any */
	if (constr)
	{
		TupleConstr *cpy = (TupleConstr *) palloc0(sizeof(TupleConstr));

		cpy->has_not_null = constr->has_not_null;
		cpy->has_generated_stored = constr->has_generated_stored;

		if ((cpy->num_defval = constr->num_defval) > 0)
		{
			cpy->defval = (AttrDefault *) palloc(cpy->num_defval * sizeof(AttrDefault));
			memcpy(cpy->defval, constr->defval, cpy->num_defval * sizeof(AttrDefault));
			for (i = cpy->num_defval - 1; i >= 0; i--)
				cpy->defval[i].adbin = pstrdup(constr->defval[i].adbin);
		}

		if (constr->missing)
		{
			cpy->missing = (AttrMissing *) palloc(tupdesc->natts * sizeof(AttrMissing));
			memcpy(cpy->missing, constr->missing, tupdesc->natts * sizeof(AttrMissing));
			for (i = tupdesc->natts - 1; i >= 0; i--)
			{
				if (constr->missing[i].am_present)
				{
					CompactAttribute *attr = TupleDescCompactAttr(tupdesc, i);

					cpy->missing[i].am_value = datumCopy(constr->missing[i].am_value,
														 attr->attbyval,
														 attr->attlen);
				}
			}
		}

		if ((cpy->num_check = constr->num_check) > 0)
		{
			cpy->check = (ConstrCheck *) palloc(cpy->num_check * sizeof(ConstrCheck));
			memcpy(cpy->check, constr->check, cpy->num_check * sizeof(ConstrCheck));
			for (i = cpy->num_check - 1; i >= 0; i--)
			{
				cpy->check[i].ccname = pstrdup(constr->check[i].ccname);
				cpy->check[i].ccbin = pstrdup(constr->check[i].ccbin);
				cpy->check[i].ccenforced = constr->check[i].ccenforced;
				cpy->check[i].ccvalid = constr->check[i].ccvalid;
				cpy->check[i].ccnoinherit = constr->check[i].ccnoinherit;
			}
		}

		desc->constr = cpy;
	}

	/* We can copy the tuple type identification, too */
	desc->tdtypeid = tupdesc->tdtypeid;
	desc->tdtypmod = tupdesc->tdtypmod;

	return desc;
}

/*
 * TupleDescCopy
 *		Copy a tuple descriptor into caller-supplied memory.
 *		The memory may be shared memory mapped at any address, and must
 *		be sufficient to hold TupleDescSize(src) bytes.
 *
 * !!! Constraints and defaults are not copied !!!
 */
void
TupleDescCopy(TupleDesc dst, TupleDesc src)
{
	int			i;

	/* Flat-copy the header and attribute arrays */
	memcpy(dst, src, TupleDescSize(src));

	/*
	 * Since we're not copying constraints and defaults, clear fields
	 * associated with them.
	 */
	for (i = 0; i < dst->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(dst, i);

		att->attnotnull = false;
		att->atthasdef = false;
		att->atthasmissing = false;
		att->attidentity = '\0';
		att->attgenerated = '\0';

		populate_compact_attribute(dst, i);
	}
	dst->constr = NULL;

	/*
	 * Also, assume the destination is not to be ref-counted.  (Copying the
	 * source's refcount would be wrong in any case.)
	 */
	dst->tdrefcount = -1;
}

/*
 * TupleDescCopyEntry
 *		This function copies a single attribute structure from one tuple
 *		descriptor to another.
 *
 * !!! Constraints and defaults are not copied !!!
 */
void
TupleDescCopyEntry(TupleDesc dst, AttrNumber dstAttno,
				   TupleDesc src, AttrNumber srcAttno)
{
	Form_pg_attribute dstAtt = TupleDescAttr(dst, dstAttno - 1);
	Form_pg_attribute srcAtt = TupleDescAttr(src, srcAttno - 1);

	/*
	 * sanity checks
	 */
	Assert(PointerIsValid(src));
	Assert(PointerIsValid(dst));
	Assert(srcAttno >= 1);
	Assert(srcAttno <= src->natts);
	Assert(dstAttno >= 1);
	Assert(dstAttno <= dst->natts);

	memcpy(dstAtt, srcAtt, ATTRIBUTE_FIXED_PART_SIZE);

	dstAtt->attnum = dstAttno;

	/* since we're not copying constraints or defaults, clear these */
	dstAtt->attnotnull = false;
	dstAtt->atthasdef = false;
	dstAtt->atthasmissing = false;
	dstAtt->attidentity = '\0';
	dstAtt->attgenerated = '\0';

	populate_compact_attribute(dst, dstAttno - 1);
}

/*
 * Free a TupleDesc including all substructure
 */
void
FreeTupleDesc(TupleDesc tupdesc)
{
	int			i;

	/*
	 * Possibly this should assert tdrefcount == 0, to disallow explicit
	 * freeing of un-refcounted tupdescs?
	 */
	Assert(tupdesc->tdrefcount <= 0);

	if (tupdesc->constr)
	{
		if (tupdesc->constr->num_defval > 0)
		{
			AttrDefault *attrdef = tupdesc->constr->defval;

			for (i = tupdesc->constr->num_defval - 1; i >= 0; i--)
				pfree(attrdef[i].adbin);
			pfree(attrdef);
		}
		if (tupdesc->constr->missing)
		{
			AttrMissing *attrmiss = tupdesc->constr->missing;

			for (i = tupdesc->natts - 1; i >= 0; i--)
			{
				if (attrmiss[i].am_present
					&& !TupleDescAttr(tupdesc, i)->attbyval)
					pfree(DatumGetPointer(attrmiss[i].am_value));
			}
			pfree(attrmiss);
		}
		if (tupdesc->constr->num_check > 0)
		{
			ConstrCheck *check = tupdesc->constr->check;

			for (i = tupdesc->constr->num_check - 1; i >= 0; i--)
			{
				pfree(check[i].ccname);
				pfree(check[i].ccbin);
			}
			pfree(check);
		}
		pfree(tupdesc->constr);
	}

	pfree(tupdesc);
}

/*
 * Increment the reference count of a tupdesc, and log the reference in
 * CurrentResourceOwner.
 *
 * Do not apply this to tupdescs that are not being refcounted.  (Use the
 * macro PinTupleDesc for tupdescs of uncertain status.)
 */
void
IncrTupleDescRefCount(TupleDesc tupdesc)
{
	Assert(tupdesc->tdrefcount >= 0);

	ResourceOwnerEnlarge(CurrentResourceOwner);
	tupdesc->tdrefcount++;
	ResourceOwnerRememberTupleDesc(CurrentResourceOwner, tupdesc);
}

/*
 * Decrement the reference count of a tupdesc, remove the corresponding
 * reference from CurrentResourceOwner, and free the tupdesc if no more
 * references remain.
 *
 * Do not apply this to tupdescs that are not being refcounted.  (Use the
 * macro ReleaseTupleDesc for tupdescs of uncertain status.)
 */
void
DecrTupleDescRefCount(TupleDesc tupdesc)
{
	Assert(tupdesc->tdrefcount > 0);

	ResourceOwnerForgetTupleDesc(CurrentResourceOwner, tupdesc);
	if (--tupdesc->tdrefcount == 0)
		FreeTupleDesc(tupdesc);
}

/*
 * Compare two TupleDesc structures for logical equality
 */
bool
equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2)
{
	int			i,
				n;

	if (tupdesc1->natts != tupdesc2->natts)
		return false;
	if (tupdesc1->tdtypeid != tupdesc2->tdtypeid)
		return false;

	/* tdtypmod and tdrefcount are not checked */

	for (i = 0; i < tupdesc1->natts; i++)
	{
		Form_pg_attribute attr1 = TupleDescAttr(tupdesc1, i);
		Form_pg_attribute attr2 = TupleDescAttr(tupdesc2, i);

		/*
		 * We do not need to check every single field here: we can disregard
		 * attrelid and attnum (which were used to place the row in the attrs
		 * array in the first place).  It might look like we could dispense
		 * with checking attlen/attbyval/attalign, since these are derived
		 * from atttypid; but in the case of dropped columns we must check
		 * them (since atttypid will be zero for all dropped columns) and in
		 * general it seems safer to check them always.
		 *
		 * We intentionally ignore atthasmissing, since that's not very
		 * relevant in tupdescs, which lack the attmissingval field.
		 */
		if (strcmp(NameStr(attr1->attname), NameStr(attr2->attname)) != 0)
			return false;
		if (attr1->atttypid != attr2->atttypid)
			return false;
		if (attr1->attlen != attr2->attlen)
			return false;
		if (attr1->attndims != attr2->attndims)
			return false;
		if (attr1->atttypmod != attr2->atttypmod)
			return false;
		if (attr1->attbyval != attr2->attbyval)
			return false;
		if (attr1->attalign != attr2->attalign)
			return false;
		if (attr1->attstorage != attr2->attstorage)
			return false;
		if (attr1->attcompression != attr2->attcompression)
			return false;
		if (attr1->attnotnull != attr2->attnotnull)
			return false;
		if (attr1->atthasdef != attr2->atthasdef)
			return false;
		if (attr1->attidentity != attr2->attidentity)
			return false;
		if (attr1->attgenerated != attr2->attgenerated)
			return false;
		if (attr1->attisdropped != attr2->attisdropped)
			return false;
		if (attr1->attislocal != attr2->attislocal)
			return false;
		if (attr1->attinhcount != attr2->attinhcount)
			return false;
		if (attr1->attcollation != attr2->attcollation)
			return false;
		/* variable-length fields are not even present... */
	}

	if (tupdesc1->constr != NULL)
	{
		TupleConstr *constr1 = tupdesc1->constr;
		TupleConstr *constr2 = tupdesc2->constr;

		if (constr2 == NULL)
			return false;
		if (constr1->has_not_null != constr2->has_not_null)
			return false;
		if (constr1->has_generated_stored != constr2->has_generated_stored)
			return false;
		n = constr1->num_defval;
		if (n != (int) constr2->num_defval)
			return false;
		/* We assume here that both AttrDefault arrays are in adnum order */
		for (i = 0; i < n; i++)
		{
			AttrDefault *defval1 = constr1->defval + i;
			AttrDefault *defval2 = constr2->defval + i;

			if (defval1->adnum != defval2->adnum)
				return false;
			if (strcmp(defval1->adbin, defval2->adbin) != 0)
				return false;
		}
		if (constr1->missing)
		{
			if (!constr2->missing)
				return false;
			for (i = 0; i < tupdesc1->natts; i++)
			{
				AttrMissing *missval1 = constr1->missing + i;
				AttrMissing *missval2 = constr2->missing + i;

				if (missval1->am_present != missval2->am_present)
					return false;
				if (missval1->am_present)
				{
					CompactAttribute *missatt1 = TupleDescCompactAttr(tupdesc1, i);

					if (!datumIsEqual(missval1->am_value, missval2->am_value,
									  missatt1->attbyval, missatt1->attlen))
						return false;
				}
			}
		}
		else if (constr2->missing)
			return false;
		n = constr1->num_check;
		if (n != (int) constr2->num_check)
			return false;

		/*
		 * Similarly, we rely here on the ConstrCheck entries being sorted by
		 * name.  If there are duplicate names, the outcome of the comparison
		 * is uncertain, but that should not happen.
		 */
		for (i = 0; i < n; i++)
		{
			ConstrCheck *check1 = constr1->check + i;
			ConstrCheck *check2 = constr2->check + i;

			if (!(strcmp(check1->ccname, check2->ccname) == 0 &&
				  strcmp(check1->ccbin, check2->ccbin) == 0 &&
				  check1->ccenforced == check2->ccenforced &&
				  check1->ccvalid == check2->ccvalid &&
				  check1->ccnoinherit == check2->ccnoinherit))
				return false;
		}
	}
	else if (tupdesc2->constr != NULL)
		return false;
	return true;
}

/*
 * equalRowTypes
 *
 * This determines whether two tuple descriptors have equal row types.  This
 * only checks those fields in pg_attribute that are applicable for row types,
 * while ignoring those fields that define the physical row storage or those
 * that define table column metadata.
 *
 * Specifically, this checks:
 *
 * - same number of attributes
 * - same composite type ID (but could both be zero)
 * - corresponding attributes (in order) have same the name, type, typmod,
 *   collation
 *
 * This is used to check whether two record types are compatible, whether
 * function return row types are the same, and other similar situations.
 *
 * (XXX There was some discussion whether attndims should be checked here, but
 * for now it has been decided not to.)
 *
 * Note: We deliberately do not check the tdtypmod field.  This allows
 * typcache.c to use this routine to see if a cached record type matches a
 * requested type.
 */
bool
equalRowTypes(TupleDesc tupdesc1, TupleDesc tupdesc2)
{
	if (tupdesc1->natts != tupdesc2->natts)
		return false;
	if (tupdesc1->tdtypeid != tupdesc2->tdtypeid)
		return false;

	for (int i = 0; i < tupdesc1->natts; i++)
	{
		Form_pg_attribute attr1 = TupleDescAttr(tupdesc1, i);
		Form_pg_attribute attr2 = TupleDescAttr(tupdesc2, i);

		if (strcmp(NameStr(attr1->attname), NameStr(attr2->attname)) != 0)
			return false;
		if (attr1->atttypid != attr2->atttypid)
			return false;
		if (attr1->atttypmod != attr2->atttypmod)
			return false;
		if (attr1->attcollation != attr2->attcollation)
			return false;

		/* Record types derived from tables could have dropped fields. */
		if (attr1->attisdropped != attr2->attisdropped)
			return false;
	}

	return true;
}

/*
 * hashRowType
 *
 * If two tuple descriptors would be considered equal by equalRowTypes()
 * then their hash value will be equal according to this function.
 */
uint32
hashRowType(TupleDesc desc)
{
	uint32		s;
	int			i;

	s = hash_combine(0, hash_uint32(desc->natts));
	s = hash_combine(s, hash_uint32(desc->tdtypeid));
	for (i = 0; i < desc->natts; ++i)
		s = hash_combine(s, hash_uint32(TupleDescAttr(desc, i)->atttypid));

	return s;
}

/*
 * TupleDescInitEntry
 *		This function initializes a single attribute structure in
 *		a previously allocated tuple descriptor.
 *
 * If attributeName is NULL, the attname field is set to an empty string
 * (this is for cases where we don't know or need a name for the field).
 * Also, some callers use this function to change the datatype-related fields
 * in an existing tupdesc; they pass attributeName = NameStr(att->attname)
 * to indicate that the attname field shouldn't be modified.
 *
 * Note that attcollation is set to the default for the specified datatype.
 * If a nondefault collation is needed, insert it afterwards using
 * TupleDescInitEntryCollation.
 */
void
TupleDescInitEntry(TupleDesc desc,
				   AttrNumber attributeNumber,
				   const char *attributeName,
				   Oid oidtypeid,
				   int32 typmod,
				   int attdim)
{
	HeapTuple	tuple;
	Form_pg_type typeForm;
	Form_pg_attribute att;

	/*
	 * sanity checks
	 */
	Assert(PointerIsValid(desc));
	Assert(attributeNumber >= 1);
	Assert(attributeNumber <= desc->natts);
	Assert(attdim >= 0);
	Assert(attdim <= PG_INT16_MAX);

	/*
	 * initialize the attribute fields
	 */
	att = TupleDescAttr(desc, attributeNumber - 1);

	att->attrelid = 0;			/* dummy value */

	/*
	 * Note: attributeName can be NULL, because the planner doesn't always
	 * fill in valid resname values in targetlists, particularly for resjunk
	 * attributes. Also, do nothing if caller wants to re-use the old attname.
	 */
	if (attributeName == NULL)
		MemSet(NameStr(att->attname), 0, NAMEDATALEN);
	else if (attributeName != NameStr(att->attname))
		namestrcpy(&(att->attname), attributeName);

	att->atttypmod = typmod;

	att->attnum = attributeNumber;
	att->attndims = attdim;

	att->attnotnull = false;
	att->atthasdef = false;
	att->atthasmissing = false;
	att->attidentity = '\0';
	att->attgenerated = '\0';
	att->attisdropped = false;
	att->attislocal = true;
	att->attinhcount = 0;
	/* variable-length fields are not present in tupledescs */

	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(oidtypeid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", oidtypeid);
	typeForm = (Form_pg_type) GETSTRUCT(tuple);

	att->atttypid = oidtypeid;
	att->attlen = typeForm->typlen;
	att->attbyval = typeForm->typbyval;
	att->attalign = typeForm->typalign;
	att->attstorage = typeForm->typstorage;
	att->attcompression = InvalidCompressionMethod;
	att->attcollation = typeForm->typcollation;

	populate_compact_attribute(desc, attributeNumber - 1);

	ReleaseSysCache(tuple);
}

/*
 * TupleDescInitBuiltinEntry
 *		Initialize a tuple descriptor without catalog access.  Only
 *		a limited range of builtin types are supported.
 */
void
TupleDescInitBuiltinEntry(TupleDesc desc,
						  AttrNumber attributeNumber,
						  const char *attributeName,
						  Oid oidtypeid,
						  int32 typmod,
						  int attdim)
{
	Form_pg_attribute att;

	/* sanity checks */
	Assert(PointerIsValid(desc));
	Assert(attributeNumber >= 1);
	Assert(attributeNumber <= desc->natts);
	Assert(attdim >= 0);
	Assert(attdim <= PG_INT16_MAX);

	/* initialize the attribute fields */
	att = TupleDescAttr(desc, attributeNumber - 1);
	att->attrelid = 0;			/* dummy value */

	/* unlike TupleDescInitEntry, we require an attribute name */
	Assert(attributeName != NULL);
	namestrcpy(&(att->attname), attributeName);

	att->atttypmod = typmod;

	att->attnum = attributeNumber;
	att->attndims = attdim;

	att->attnotnull = false;
	att->atthasdef = false;
	att->atthasmissing = false;
	att->attidentity = '\0';
	att->attgenerated = '\0';
	att->attisdropped = false;
	att->attislocal = true;
	att->attinhcount = 0;
	/* variable-length fields are not present in tupledescs */

	att->atttypid = oidtypeid;

	/*
	 * Our goal here is to support just enough types to let basic builtin
	 * commands work without catalog access - e.g. so that we can do certain
	 * things even in processes that are not connected to a database.
	 */
	switch (oidtypeid)
	{
		case TEXTOID:
		case TEXTARRAYOID:
			att->attlen = -1;
			att->attbyval = false;
			att->attalign = TYPALIGN_INT;
			att->attstorage = TYPSTORAGE_EXTENDED;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = DEFAULT_COLLATION_OID;
			break;

		case BOOLOID:
			att->attlen = 1;
			att->attbyval = true;
			att->attalign = TYPALIGN_CHAR;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		case INT4OID:
			att->attlen = 4;
			att->attbyval = true;
			att->attalign = TYPALIGN_INT;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		case INT8OID:
			att->attlen = 8;
			att->attbyval = FLOAT8PASSBYVAL;
			att->attalign = TYPALIGN_DOUBLE;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		case OIDOID:
			att->attlen = 4;
			att->attbyval = true;
			att->attalign = TYPALIGN_INT;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		default:
			elog(ERROR, "unsupported type %u", oidtypeid);
	}

	populate_compact_attribute(desc, attributeNumber - 1);
}

/*
 * TupleDescInitEntryCollation
 *
 * Assign a nondefault collation to a previously initialized tuple descriptor
 * entry.
 */
void
TupleDescInitEntryCollation(TupleDesc desc,
							AttrNumber attributeNumber,
							Oid collationid)
{
	/*
	 * sanity checks
	 */
	Assert(PointerIsValid(desc));
	Assert(attributeNumber >= 1);
	Assert(attributeNumber <= desc->natts);

	TupleDescAttr(desc, attributeNumber - 1)->attcollation = collationid;
}

/*
 * BuildDescFromLists
 *
 * Build a TupleDesc given lists of column names (as String nodes),
 * column type OIDs, typmods, and collation OIDs.
 *
 * No constraints are generated.
 *
 * This is for use with functions returning RECORD.
 */
TupleDesc
BuildDescFromLists(const List *names, const List *types, const List *typmods, const List *collations)
{
	int			natts;
	AttrNumber	attnum;
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;
	ListCell   *l4;
	TupleDesc	desc;

	natts = list_length(names);
	Assert(natts == list_length(types));
	Assert(natts == list_length(typmods));
	Assert(natts == list_length(collations));

	/*
	 * allocate a new tuple descriptor
	 */
	desc = CreateTemplateTupleDesc(natts);

	attnum = 0;
	forfour(l1, names, l2, types, l3, typmods, l4, collations)
	{
		char	   *attname = strVal(lfirst(l1));
		Oid			atttypid = lfirst_oid(l2);
		int32		atttypmod = lfirst_int(l3);
		Oid			attcollation = lfirst_oid(l4);

		attnum++;

		TupleDescInitEntry(desc, attnum, attname, atttypid, atttypmod, 0);
		TupleDescInitEntryCollation(desc, attnum, attcollation);
	}

	return desc;
}

/*
 * Get default expression (or NULL if none) for the given attribute number.
 */
Node *
TupleDescGetDefault(TupleDesc tupdesc, AttrNumber attnum)
{
	Node	   *result = NULL;

	if (tupdesc->constr)
	{
		AttrDefault *attrdef = tupdesc->constr->defval;

		for (int i = 0; i < tupdesc->constr->num_defval; i++)
		{
			if (attrdef[i].adnum == attnum)
			{
				result = stringToNode(attrdef[i].adbin);
				break;
			}
		}
	}

	return result;
}

/* ResourceOwner callbacks */

static void
ResOwnerReleaseTupleDesc(Datum res)
{
	TupleDesc	tupdesc = (TupleDesc) DatumGetPointer(res);

	/* Like DecrTupleDescRefCount, but don't call ResourceOwnerForget() */
	Assert(tupdesc->tdrefcount > 0);
	if (--tupdesc->tdrefcount == 0)
		FreeTupleDesc(tupdesc);
}

static char *
ResOwnerPrintTupleDesc(Datum res)
{
	TupleDesc	tupdesc = (TupleDesc) DatumGetPointer(res);

	return psprintf("TupleDesc %p (%u,%d)",
					tupdesc, tupdesc->tdtypeid, tupdesc->tdtypmod);
}
