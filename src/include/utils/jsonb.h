/*-------------------------------------------------------------------------
 *
 * jsonb.h
 *	  Declarations for jsonb data type support.
 *
 * Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 * src/include/utils/jsonb.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef __JSONB_H__
#define __JSONB_H__

#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/numeric.h"

/*
 * JB_CMASK is used to extract count of items
 *
 * It's not possible to get more than 2^28 items into an Jsonb.
 */
#define JB_CMASK				0x0FFFFFFF

#define JB_FSCALAR				0x10000000
#define JB_FOBJECT				0x20000000
#define JB_FARRAY				0x40000000

/* Get information on varlena Jsonb */
#define JB_ROOT_COUNT(jbp_)		( *(uint32*) VARDATA(jbp_) & JB_CMASK)
#define JB_ROOT_IS_SCALAR(jbp_)	( *(uint32*) VARDATA(jbp_) & JB_FSCALAR)
#define JB_ROOT_IS_OBJECT(jbp_)	( *(uint32*) VARDATA(jbp_) & JB_FOBJECT)
#define JB_ROOT_IS_ARRAY(jbp_)	( *(uint32*) VARDATA(jbp_) & JB_FARRAY)

/* Jentry macros */
#define JENTRY_POSMASK			0x0FFFFFFF
#define JENTRY_ISFIRST			0x80000000
#define JENTRY_TYPEMASK 		(~(JENTRY_POSMASK | JENTRY_ISFIRST))
#define JENTRY_ISSTRING			0x00000000
#define JENTRY_ISNUMERIC		0x10000000
#define JENTRY_ISNEST			0x20000000
#define JENTRY_ISNULL			0x40000000
#define JENTRY_ISBOOL			(JENTRY_ISNUMERIC | JENTRY_ISNEST)
#define JENTRY_ISFALSE			JENTRY_ISBOOL
#define JENTRY_ISTRUE			(JENTRY_ISBOOL | 0x40000000)
/* Note possible multiple evaluations, also access to prior array element */
#define JBE_ISFIRST(je_)		(((je_).header & JENTRY_ISFIRST) != 0)
#define JBE_ISSTRING(je_)		(((je_).header & JENTRY_TYPEMASK) == JENTRY_ISSTRING)
#define JBE_ISNUMERIC(je_)		(((je_).header & JENTRY_TYPEMASK) == JENTRY_ISNUMERIC)
#define JBE_ISNEST(je_)			(((je_).header & JENTRY_TYPEMASK) == JENTRY_ISNEST)
#define JBE_ISNULL(je_)			(((je_).header & JENTRY_TYPEMASK) == JENTRY_ISNULL)
#define JBE_ISBOOL(je_)			(((je_).header & JENTRY_TYPEMASK & JENTRY_ISBOOL) == JENTRY_ISBOOL)
#define JBE_ISBOOL_TRUE(je_)	(((je_).header & JENTRY_TYPEMASK) == JENTRY_ISTRUE)
#define JBE_ISBOOL_FALSE(je_)	(JBE_ISBOOL(je_) && !JBE_ISBOOL_TRUE(je_))

/* Get offset for Jentry  */
#define JBE_ENDPOS(je_) 		((je_).header & JENTRY_POSMASK)
#define JBE_OFF(je_) 			(JBE_ISFIRST(je_) ? 0 : JBE_ENDPOS((&(je_))[-1]))
#define JBE_LEN(je_) 			(JBE_ISFIRST(je_) ? \
								 JBE_ENDPOS(je_) \
								 : JBE_ENDPOS(je_) - JBE_ENDPOS((&(je_))[-1]))

/* Flags indicating a stage of sequential Jsonb processing */
#define WJB_DONE				0x000
#define WJB_KEY					0x001
#define WJB_VALUE				0x002
#define WJB_ELEM				0x004
#define WJB_BEGIN_ARRAY			0x008
#define WJB_END_ARRAY			0x010
#define WJB_BEGIN_OBJECT		0x020
#define WJB_END_OBJECT			0x040

/*
 * When using a GIN index for jsonb, we choose to index both keys and values.
 * The storage format is text, with K, or V prepended to the string to indicate
 * key/element or value/element.
 *
 * Jsonb Keys and string array elements are treated equivalently when
 * serialized to text index storage.  One day we may wish to create an opclass
 * that only indexes values, but for now keys and values are stored in GIN
 * indexes in a way that doesn't really consider their relationship to each
 * other.
 */
#define JKEYELEM	'K'
#define JVAL		'V'

#define JsonbContainsStrategyNumber		7
#define JsonbExistsStrategyNumber		9
#define JsonbExistsAnyStrategyNumber	10
#define JsonbExistsAllStrategyNumber	11

/* Convenience macros */
#define DatumGetJsonb(d)	((Jsonb *) PG_DETOAST_DATUM(d))
#define JsonbGetDatum(p)	PointerGetDatum(p)
#define PG_GETARG_JSONB(x)	DatumGetJsonb(PG_GETARG_DATUM(x))
#define PG_RETURN_JSONB(x)	PG_RETURN_POINTER(x)

typedef struct JsonbPair JsonbPair;
typedef struct JsonbValue JsonbValue;
typedef	char*  JsonbSuperHeader;

/*
 * Jsonbs are varlena objects, so must meet the varlena convention that the
 * first int32 of the object contains the total object size in bytes.  Be sure
 * to use VARSIZE() and SET_VARSIZE() to access it, though!
 *
 * Jsonb is the on-disk representation, in contrast to the in-memory JsonbValue
 * representation.  Often, JsonbValues are just shims through which a Jsonb
 * buffer is accessed, but they can also be deep copied and passed around.
 *
 * We have an abstraction called a "superheader".  This is a pointer that
 * conventionally points to the first item after our 4-byte uncompressed
 * varlena header, from which we can read flags using bitwise operations.
 *
 * Frequently, we pass a superheader reference to a function, and it doesn't
 * matter if it points to just after the start of a Jsonb, or to a temp buffer.
 */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint32		superheader;
	/* (array of JEntry follows, size determined using uint32 superheader) */
} Jsonb;

/*
 * JEntry: there is one of these for each key _and_ value for objects.  Arrays
 * have one per element.
 *
 * The position offset points to the _end_ so that we can get the length by
 * subtraction from the previous entry.	 The JENTRY_ISFIRST flag indicates if
 * there is a previous entry.
 */
typedef struct
{
	uint32		header;		/* Shares some flags with superheader */
}	JEntry;

#define IsAJsonbScalar(jsonbval)	((jsonbval)->type >= jbvNull && \
									 (jsonbval)->type <= jbvBool)

/*
 * JsonbValue:  In-memory representation of Jsonb.  This is a convenient
 * deserialized representation, that can easily support using the "val"
 * union across underlying types during manipulation.  The Jsonb on-disk
 * representation has various alignment considerations.
 */
struct JsonbValue
{
	enum
	{
		/* Scalar types */
		jbvNull = 0x0,
		jbvString,
		jbvNumeric,
		jbvBool,
		/* Composite types */
		jbvArray = 0x10,
		jbvObject,
		/* Binary (i.e. struct Jsonb) jbvArray/jbvObject */
		jbvBinary
	} type;		/* Influences sort order */

	int				estSize;	/* Estimated size of node (including
								 * subnodes) */

	union
	{
		Numeric		numeric;
		bool		boolean;
		struct
		{
			int			len;
			char	   *val;		/* Not necessarily null-terminated */
		} string;		/* String primitive type */

		struct
		{
			int			nElems;
			JsonbValue *elems;
			bool		rawScalar;	/* Top-level "raw scalar" array? */
		} array;		/* Array container type */

		struct
		{
			int			nPairs;		/* 1 pair, 2 elements */
			JsonbPair  *pairs;
		} object;		/* Associative container type */

		struct
		{
			int			len;
			char	   *data;
		} binary;
	} val;
};

/*
 * Pair within an Object.
 *
 * Pairs with duplicate keys are de-duplicated.  We store the order for the
 * benefit of doing so in a well-defined way with respect to the original
 * observed order (which is "last observed wins").  This is only used briefly
 * when originally constructing a Jsonb.
 */
struct JsonbPair
{
	JsonbValue	key;			/* Must be a jbvString */
	JsonbValue	value;			/* May be of any type */
	uint32		order;			/* preserves order of pairs with equal keys */
};

/* Conversion state used when parsing Jsonb from text, or for type coercion */
typedef struct JsonbParseState
{
	JsonbValue	contVal;
	Size		size;
	struct JsonbParseState *next;
} JsonbParseState;

/*
 * JsonbIterator holds details of the type for each iteration. It also stores a
 * Jsonb varlena buffer, which can be directly accessed in some contexts.
 */
typedef enum
{
	jbi_start = 0x0,
	jbi_key,
	jbi_value,
	jbi_elem
} JsonbIterState;

typedef struct JsonbIterator
{
	/* Jsonb varlena buffer (may or may not be root) */
	char	   *buffer;

	/* Current value */
	uint32		containerType; /* Never of value JB_FSCALAR, since
								* scalars will appear in pseudo-arrays */
	uint32		nElems;		   /* Number of elements in metaArray
								* (will be nPairs for objects) */
	bool		isScalar;	   /* Pseudo-array scalar value? */
	JEntry	   *meta;

	/* Current item in buffer (up to nElems, but must * 2 for objects) */
	int			i;

	/*
	 * Data proper.  Note that this points just past end of "meta" array.  We
	 * use its metadata (Jentrys) with JBE_OFF() macro to find appropriate
	 * offsets into this array.
	 */
	char	   *dataProper;

	/* Private state */
	JsonbIterState state;

	struct JsonbIterator *parent;
} JsonbIterator;

/* I/O routines */
extern Datum jsonb_in(PG_FUNCTION_ARGS);
extern Datum jsonb_out(PG_FUNCTION_ARGS);
extern Datum jsonb_recv(PG_FUNCTION_ARGS);
extern Datum jsonb_send(PG_FUNCTION_ARGS);
extern Datum jsonb_typeof(PG_FUNCTION_ARGS);

/* Indexing-related ops */
extern Datum jsonb_exists(PG_FUNCTION_ARGS);
extern Datum jsonb_exists_any(PG_FUNCTION_ARGS);
extern Datum jsonb_exists_all(PG_FUNCTION_ARGS);
extern Datum jsonb_contains(PG_FUNCTION_ARGS);
extern Datum jsonb_contained(PG_FUNCTION_ARGS);
extern Datum jsonb_ne(PG_FUNCTION_ARGS);
extern Datum jsonb_lt(PG_FUNCTION_ARGS);
extern Datum jsonb_gt(PG_FUNCTION_ARGS);
extern Datum jsonb_le(PG_FUNCTION_ARGS);
extern Datum jsonb_ge(PG_FUNCTION_ARGS);
extern Datum jsonb_eq(PG_FUNCTION_ARGS);
extern Datum jsonb_cmp(PG_FUNCTION_ARGS);
extern Datum jsonb_hash(PG_FUNCTION_ARGS);

/* GIN support functions */
extern Datum gin_compare_jsonb(PG_FUNCTION_ARGS);
extern Datum gin_extract_jsonb(PG_FUNCTION_ARGS);
extern Datum gin_extract_jsonb_query(PG_FUNCTION_ARGS);
extern Datum gin_consistent_jsonb(PG_FUNCTION_ARGS);
extern Datum gin_triconsistent_jsonb(PG_FUNCTION_ARGS);
/* GIN hash opclass functions */
extern Datum gin_extract_jsonb_hash(PG_FUNCTION_ARGS);
extern Datum gin_extract_jsonb_query_hash(PG_FUNCTION_ARGS);
extern Datum gin_consistent_jsonb_hash(PG_FUNCTION_ARGS);
extern Datum gin_triconsistent_jsonb_hash(PG_FUNCTION_ARGS);

/* Support functions */
extern int	compareJsonbSuperHeaderValue(JsonbSuperHeader a,
										 JsonbSuperHeader b);
extern JsonbValue *findJsonbValueFromSuperHeader(JsonbSuperHeader sheader,
												 uint32 flags,
												 uint32 *lowbound,
												 JsonbValue *key);
extern JsonbValue *getIthJsonbValueFromSuperHeader(JsonbSuperHeader sheader,
												   uint32 i);
extern JsonbValue *pushJsonbValue(JsonbParseState ** pstate, int seq,
								  JsonbValue *scalarVal);
extern JsonbIterator *JsonbIteratorInit(JsonbSuperHeader buffer);
extern int JsonbIteratorNext(JsonbIterator **it, JsonbValue *val,
							 bool skipNested);
extern Jsonb *JsonbValueToJsonb(JsonbValue *val);
extern bool JsonbDeepContains(JsonbIterator ** val,
							  JsonbIterator ** mContained);
extern JsonbValue *arrayToJsonbSortedArray(ArrayType *a);
extern void JsonbHashScalarValue(const JsonbValue * scalarVal, uint32 * hash);

/* jsonb.c support function */
extern char *JsonbToCString(StringInfo out, JsonbSuperHeader in,
							int estimated_len);

#endif   /* __JSONB_H__ */
