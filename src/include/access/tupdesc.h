/*-------------------------------------------------------------------------
 *
 * tupdesc.h
 *	  POSTGRES tuple descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupdesc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPDESC_H
#define TUPDESC_H

#include "access/attnum.h"
#include "catalog/pg_attribute.h"
#include "nodes/pg_list.h"


typedef struct AttrDefault
{
	AttrNumber	adnum;
	char	   *adbin;			/* nodeToString representation of expr */
} AttrDefault;

typedef struct ConstrCheck
{
	char	   *ccname;
	char	   *ccbin;			/* nodeToString representation of expr */
	bool		ccenforced;
	bool		ccvalid;
	bool		ccnoinherit;	/* this is a non-inheritable constraint */
} ConstrCheck;

/* This structure contains constraints of a tuple */
typedef struct TupleConstr
{
	AttrDefault *defval;		/* array */
	ConstrCheck *check;			/* array */
	struct AttrMissing *missing;	/* missing attributes values, NULL if none */
	uint16		num_defval;
	uint16		num_check;
	bool		has_not_null;
	bool		has_generated_stored;
} TupleConstr;

/*
 * CompactAttribute
 *		Cut-down version of FormData_pg_attribute for faster access for tasks
 *		such as tuple deformation.  The fields of this struct are populated
 *		using the populate_compact_attribute() function, which must be called
 *		directly after the FormData_pg_attribute struct is populated or
 *		altered in any way.
 *
 * Currently, this struct is 16 bytes.  Any code changes which enlarge this
 * struct should be considered very carefully.
 *
 * Code which must access a TupleDesc's attribute data should always make use
 * the fields of this struct when required fields are available here.  It's
 * more efficient to access the memory in CompactAttribute due to it being a
 * more compact representation of FormData_pg_attribute and also because
 * accessing the FormData_pg_attribute requires an additional calculations to
 * obtain the base address of the array within the TupleDesc.
 */
typedef struct CompactAttribute
{
	int32		attcacheoff;	/* fixed offset into tuple, if known, or -1 */
	int16		attlen;			/* attr len in bytes or -1 = varlen, -2 =
								 * cstring */
	bool		attbyval;		/* as FormData_pg_attribute.attbyval */
	bool		attispackable;	/* FormData_pg_attribute.attstorage !=
								 * TYPSTORAGE_PLAIN */
	bool		atthasmissing;	/* as FormData_pg_attribute.atthasmissing */
	bool		attisdropped;	/* as FormData_pg_attribute.attisdropped */
	bool		attgenerated;	/* FormData_pg_attribute.attgenerated != '\0' */
	bool		attnotnull;		/* as FormData_pg_attribute.attnotnull */
	uint8		attalignby;		/* alignment requirement in bytes */
} CompactAttribute;

/*
 * This struct is passed around within the backend to describe the structure
 * of tuples.  For tuples coming from on-disk relations, the information is
 * collected from the pg_attribute, pg_attrdef, and pg_constraint catalogs.
 * Transient row types (such as the result of a join query) have anonymous
 * TupleDesc structs that generally omit any constraint info; therefore the
 * structure is designed to let the constraints be omitted efficiently.
 *
 * Note that only user attributes, not system attributes, are mentioned in
 * TupleDesc.
 *
 * If the tupdesc is known to correspond to a named rowtype (such as a table's
 * rowtype) then tdtypeid identifies that type and tdtypmod is -1.  Otherwise
 * tdtypeid is RECORDOID, and tdtypmod can be either -1 for a fully anonymous
 * row type, or a value >= 0 to allow the rowtype to be looked up in the
 * typcache.c type cache.
 *
 * Note that tdtypeid is never the OID of a domain over composite, even if
 * we are dealing with values that are known (at some higher level) to be of
 * a domain-over-composite type.  This is because tdtypeid/tdtypmod need to
 * match up with the type labeling of composite Datums, and those are never
 * explicitly marked as being of a domain type, either.
 *
 * Tuple descriptors that live in caches (relcache or typcache, at present)
 * are reference-counted: they can be deleted when their reference count goes
 * to zero.  Tuple descriptors created by the executor need no reference
 * counting, however: they are simply created in the appropriate memory
 * context and go away when the context is freed.  We set the tdrefcount
 * field of such a descriptor to -1, while reference-counted descriptors
 * always have tdrefcount >= 0.
 *
 * Beyond the compact_attrs variable length array, the TupleDesc stores an
 * array of FormData_pg_attribute.  The TupleDescAttr() function, as defined
 * below, takes care of calculating the address of the elements of the
 * FormData_pg_attribute array.
 *
 * The array of CompactAttribute is effectively an abbreviated version of the
 * array of FormData_pg_attribute.  Because CompactAttribute is significantly
 * smaller than FormData_pg_attribute, code, especially performance-critical
 * code, should prioritize using the fields from the CompactAttribute over the
 * equivalent fields in FormData_pg_attribute.
 *
 * Any code making changes manually to and fields in the FormData_pg_attribute
 * array must subsequently call populate_compact_attribute() to flush the
 * changes out to the corresponding 'compact_attrs' element.
 */
typedef struct TupleDescData
{
	int			natts;			/* number of attributes in the tuple */
	Oid			tdtypeid;		/* composite type ID for tuple type */
	int32		tdtypmod;		/* typmod for tuple type */
	int			tdrefcount;		/* reference count, or -1 if not counting */
	TupleConstr *constr;		/* constraints, or NULL if none */
	/* compact_attrs[N] is the compact metadata of Attribute Number N+1 */
	CompactAttribute compact_attrs[FLEXIBLE_ARRAY_MEMBER];
}			TupleDescData;
typedef struct TupleDescData *TupleDesc;

extern void populate_compact_attribute(TupleDesc tupdesc, int attnum);

/*
 * Calculates the base address of the Form_pg_attribute at the end of the
 * TupleDescData struct.
 */
#define TupleDescAttrAddress(desc) \
	(Form_pg_attribute) ((char *) (desc) + \
	 (offsetof(struct TupleDescData, compact_attrs) + \
	 (desc)->natts * sizeof(CompactAttribute)))

/* Accessor for the i'th FormData_pg_attribute element of tupdesc. */
static inline FormData_pg_attribute *
TupleDescAttr(TupleDesc tupdesc, int i)
{
	FormData_pg_attribute *attrs = TupleDescAttrAddress(tupdesc);

	return &attrs[i];
}

#undef TupleDescAttrAddress

extern void verify_compact_attribute(TupleDesc, int attnum);

/*
 * Accessor for the i'th CompactAttribute element of tupdesc.
 */
static inline CompactAttribute *
TupleDescCompactAttr(TupleDesc tupdesc, int i)
{
	CompactAttribute *cattr = &tupdesc->compact_attrs[i];

#ifdef USE_ASSERT_CHECKING

	/* Check that the CompactAttribute is correctly populated */
	verify_compact_attribute(tupdesc, i);
#endif

	return cattr;
}

extern TupleDesc CreateTemplateTupleDesc(int natts);

extern TupleDesc CreateTupleDesc(int natts, Form_pg_attribute *attrs);

extern TupleDesc CreateTupleDescCopy(TupleDesc tupdesc);

extern TupleDesc CreateTupleDescTruncatedCopy(TupleDesc tupdesc, int natts);

extern TupleDesc CreateTupleDescCopyConstr(TupleDesc tupdesc);

#define TupleDescSize(src) \
	(offsetof(struct TupleDescData, compact_attrs) + \
	 (src)->natts * sizeof(CompactAttribute) + \
	 (src)->natts * sizeof(FormData_pg_attribute))

extern void TupleDescCopy(TupleDesc dst, TupleDesc src);

extern void TupleDescCopyEntry(TupleDesc dst, AttrNumber dstAttno,
							   TupleDesc src, AttrNumber srcAttno);

extern void FreeTupleDesc(TupleDesc tupdesc);

extern void IncrTupleDescRefCount(TupleDesc tupdesc);
extern void DecrTupleDescRefCount(TupleDesc tupdesc);

#define PinTupleDesc(tupdesc) \
	do { \
		if ((tupdesc)->tdrefcount >= 0) \
			IncrTupleDescRefCount(tupdesc); \
	} while (0)

#define ReleaseTupleDesc(tupdesc) \
	do { \
		if ((tupdesc)->tdrefcount >= 0) \
			DecrTupleDescRefCount(tupdesc); \
	} while (0)

extern bool equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);
extern bool equalRowTypes(TupleDesc tupdesc1, TupleDesc tupdesc2);
extern uint32 hashRowType(TupleDesc desc);

extern void TupleDescInitEntry(TupleDesc desc,
							   AttrNumber attributeNumber,
							   const char *attributeName,
							   Oid oidtypeid,
							   int32 typmod,
							   int attdim);

extern void TupleDescInitBuiltinEntry(TupleDesc desc,
									  AttrNumber attributeNumber,
									  const char *attributeName,
									  Oid oidtypeid,
									  int32 typmod,
									  int attdim);

extern void TupleDescInitEntryCollation(TupleDesc desc,
										AttrNumber attributeNumber,
										Oid collationid);

extern TupleDesc BuildDescFromLists(const List *names, const List *types, const List *typmods, const List *collations);

extern Node *TupleDescGetDefault(TupleDesc tupdesc, AttrNumber attnum);

#endif							/* TUPDESC_H */
