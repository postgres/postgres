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

/* Tokens used when sequentially processing a jsonb value */
typedef enum
{
	WJB_DONE,
	WJB_KEY,
	WJB_VALUE,
	WJB_ELEM,
	WJB_BEGIN_ARRAY,
	WJB_END_ARRAY,
	WJB_BEGIN_OBJECT,
	WJB_END_OBJECT
} JsonbIteratorToken;

/* Strategy numbers for GIN index opclasses */
#define JsonbContainsStrategyNumber		7
#define JsonbExistsStrategyNumber		9
#define JsonbExistsAnyStrategyNumber	10
#define JsonbExistsAllStrategyNumber	11

/*
 * In the standard jsonb_ops GIN opclass for jsonb, we choose to index both
 * keys and values.  The storage format is text.  The first byte of the text
 * string distinguishes whether this is a key (always a string), null value,
 * boolean value, numeric value, or string value.  However, array elements
 * that are strings are marked as though they were keys; this imprecision
 * supports the definition of the "exists" operator, which treats array
 * elements like keys.  The remainder of the text string is empty for a null
 * value, "t" or "f" for a boolean value, a normalized print representation of
 * a numeric value, or the text of a string value.  However, if the length of
 * this text representation would exceed JGIN_MAXLENGTH bytes, we instead hash
 * the text representation and store an 8-hex-digit representation of the
 * uint32 hash value, marking the prefix byte with an additional bit to
 * distinguish that this has happened.  Hashing long strings saves space and
 * ensures that we won't overrun the maximum entry length for a GIN index.
 * (But JGIN_MAXLENGTH is quite a bit shorter than GIN's limit.  It's chosen
 * to ensure that the on-disk text datum will have a short varlena header.)
 * Note that when any hashed item appears in a query, we must recheck index
 * matches against the heap tuple; currently, this costs nothing because we
 * must always recheck for other reasons.
 */
#define JGINFLAG_KEY	0x01	/* key (or string array element) */
#define JGINFLAG_NULL	0x02	/* null value */
#define JGINFLAG_BOOL	0x03	/* boolean value */
#define JGINFLAG_NUM	0x04	/* numeric value */
#define JGINFLAG_STR	0x05	/* string value (if not an array element) */
#define JGINFLAG_HASHED 0x10	/* OR'd into flag if value was hashed */
#define JGIN_MAXLENGTH	125		/* max length of text part before hashing */

/* Convenience macros */
#define DatumGetJsonb(d)	((Jsonb *) PG_DETOAST_DATUM(d))
#define JsonbGetDatum(p)	PointerGetDatum(p)
#define PG_GETARG_JSONB(x)	DatumGetJsonb(PG_GETARG_DATUM(x))
#define PG_RETURN_JSONB(x)	PG_RETURN_POINTER(x)

typedef struct JsonbPair JsonbPair;
typedef struct JsonbValue JsonbValue;

/*
 * Jsonbs are varlena objects, so must meet the varlena convention that the
 * first int32 of the object contains the total object size in bytes.  Be sure
 * to use VARSIZE() and SET_VARSIZE() to access it, though!
 *
 * Jsonb is the on-disk representation, in contrast to the in-memory JsonbValue
 * representation.  Often, JsonbValues are just shims through which a Jsonb
 * buffer is accessed, but they can also be deep copied and passed around.
 *
 * Jsonb is a tree structure. Each node in the tree consists of a JEntry
 * header, and a variable-length content.  The JEntry header indicates what
 * kind of a node it is, e.g. a string or an array, and the offset and length
 * of its variable-length portion within the container.
 *
 * The JEntry and the content of a node are not stored physically together.
 * Instead, the container array or object has an array that holds the JEntrys
 * of all the child nodes, followed by their variable-length portions.
 *
 * The root node is an exception; it has no parent array or object that could
 * hold its JEntry. Hence, no JEntry header is stored for the root node.  It
 * is implicitly known that the the root node must be an array or an object,
 * so we can get away without the type indicator as long as we can distinguish
 * the two.  For that purpose, both an array and an object begins with a uint32
 * header field, which contains an JB_FOBJECT or JB_FARRAY flag.  When a naked
 * scalar value needs to be stored as a Jsonb value, what we actually store is
 * an array with one element, with the flags in the array's header field set
 * to JB_FSCALAR | JB_FARRAY.
 *
 * To encode the length and offset of the variable-length portion of each
 * node in a compact way, the JEntry stores only the end offset within the
 * variable-length portion of the container node. For the first JEntry in the
 * container's JEntry array, that equals to the length of the node data. For
 * convenience, the JENTRY_ISFIRST flag is set. The begin offset and length
 * of the rest of the entries can be calculated using the end offset of the
 * previous JEntry in the array.
 *
 * Overall, the Jsonb struct requires 4-bytes alignment. Within the struct,
 * the variable-length portion of some node types is aligned to a 4-byte
 * boundary, while others are not. When alignment is needed, the padding is
 * in the beginning of the node that requires it. For example, if a numeric
 * node is stored after a string node, so that the numeric node begins at
 * offset 3, the variable-length portion of the numeric node will begin with
 * one padding byte.
 */

/*
 * Jentry format.
 *
 * The least significant 28 bits store the end offset of the entry (see
 * JBE_ENDPOS, JBE_OFF, JBE_LEN macros below). The next three bits
 * are used to store the type of the entry. The most significant bit
 * is set on the first entry in an array of JEntrys.
 */
typedef uint32 JEntry;

#define JENTRY_POSMASK			0x0FFFFFFF
#define JENTRY_TYPEMASK			0x70000000

/* values stored in the type bits */
#define JENTRY_ISSTRING			0x00000000
#define JENTRY_ISNUMERIC		0x10000000
#define JENTRY_ISBOOL_FALSE		0x20000000
#define JENTRY_ISBOOL_TRUE		0x30000000
#define JENTRY_ISNULL			0x40000000
#define JENTRY_ISCONTAINER		0x50000000		/* array or object */

/* Note possible multiple evaluations */
#define JBE_ISFIRST(je_)		(((je_) & JENTRY_ISFIRST) != 0)
#define JBE_ISSTRING(je_)		(((je_) & JENTRY_TYPEMASK) == JENTRY_ISSTRING)
#define JBE_ISNUMERIC(je_)		(((je_) & JENTRY_TYPEMASK) == JENTRY_ISNUMERIC)
#define JBE_ISCONTAINER(je_)	(((je_) & JENTRY_TYPEMASK) == JENTRY_ISCONTAINER)
#define JBE_ISNULL(je_)			(((je_) & JENTRY_TYPEMASK) == JENTRY_ISNULL)
#define JBE_ISBOOL_TRUE(je_)	(((je_) & JENTRY_TYPEMASK) == JENTRY_ISBOOL_TRUE)
#define JBE_ISBOOL_FALSE(je_)	(((je_) & JENTRY_TYPEMASK) == JENTRY_ISBOOL_FALSE)
#define JBE_ISBOOL(je_)			(JBE_ISBOOL_TRUE(je_) || JBE_ISBOOL_FALSE(je_))

/*
 * Macros for getting the offset and length of an element. Note multiple
 * evaluations and access to prior array element.
 */
#define JBE_ENDPOS(je_)			((je_) & JENTRY_POSMASK)
#define JBE_OFF(ja, i)			((i) == 0 ? 0 : JBE_ENDPOS((ja)[i - 1]))
#define JBE_LEN(ja, i)			((i) == 0 ? JBE_ENDPOS((ja)[i]) \
								 : JBE_ENDPOS((ja)[i]) - JBE_ENDPOS((ja)[i - 1]))

/*
 * A jsonb array or object node, within a Jsonb Datum.
 *
 * An array has one child for each element. An object has two children for
 * each key/value pair.
 */
typedef struct JsonbContainer
{
	uint32		header;			/* number of elements or key/value pairs, and
								 * flags */
	JEntry		children[1];	/* variable length */

	/* the data for each child node follows. */
} JsonbContainer;

/* flags for the header-field in JsonbContainer */
#define JB_CMASK				0x0FFFFFFF
#define JB_FSCALAR				0x10000000
#define JB_FOBJECT				0x20000000
#define JB_FARRAY				0x40000000

/* The top-level on-disk format for a jsonb datum. */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	JsonbContainer root;
} Jsonb;

/* convenience macros for accessing the root container in a Jsonb datum */
#define JB_ROOT_COUNT(jbp_)		( *(uint32*) VARDATA(jbp_) & JB_CMASK)
#define JB_ROOT_IS_SCALAR(jbp_) ( *(uint32*) VARDATA(jbp_) & JB_FSCALAR)
#define JB_ROOT_IS_OBJECT(jbp_) ( *(uint32*) VARDATA(jbp_) & JB_FOBJECT)
#define JB_ROOT_IS_ARRAY(jbp_)	( *(uint32*) VARDATA(jbp_) & JB_FARRAY)


/*
 * JsonbValue:	In-memory representation of Jsonb.  This is a convenient
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
	}			type;			/* Influences sort order */

	union
	{
		Numeric numeric;
		bool		boolean;
		struct
		{
			int			len;
			char	   *val;	/* Not necessarily null-terminated */
		}			string;		/* String primitive type */

		struct
		{
			int			nElems;
			JsonbValue *elems;
			bool		rawScalar;		/* Top-level "raw scalar" array? */
		}			array;		/* Array container type */

		struct
		{
			int			nPairs; /* 1 pair, 2 elements */
			JsonbPair  *pairs;
		}			object;		/* Associative container type */

		struct
		{
			int			len;
			JsonbContainer *data;
		}			binary;		/* Array or object, in on-disk format */
	}			val;
};

#define IsAJsonbScalar(jsonbval)	((jsonbval)->type >= jbvNull && \
									 (jsonbval)->type <= jbvBool)

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
	JBI_ARRAY_START,
	JBI_ARRAY_ELEM,
	JBI_OBJECT_START,
	JBI_OBJECT_KEY,
	JBI_OBJECT_VALUE
} JsonbIterState;

typedef struct JsonbIterator
{
	/* Container being iterated */
	JsonbContainer *container;
	uint32		nElems;			/* Number of elements in children array (will be
								 * nPairs for objects) */
	bool		isScalar;		/* Pseudo-array scalar value? */
	JEntry	   *children;

	/* Current item in buffer (up to nElems, but must * 2 for objects) */
	int			i;

	/*
	 * Data proper.  This points just past end of children array.
	 * We use the JBE_OFF() macro on the Jentrys to find offsets of each
	 * child in this area.
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

/* GIN support functions for jsonb_ops */
extern Datum gin_compare_jsonb(PG_FUNCTION_ARGS);
extern Datum gin_extract_jsonb(PG_FUNCTION_ARGS);
extern Datum gin_extract_jsonb_query(PG_FUNCTION_ARGS);
extern Datum gin_consistent_jsonb(PG_FUNCTION_ARGS);
extern Datum gin_triconsistent_jsonb(PG_FUNCTION_ARGS);

/* GIN support functions for jsonb_path_ops */
extern Datum gin_extract_jsonb_path(PG_FUNCTION_ARGS);
extern Datum gin_extract_jsonb_query_path(PG_FUNCTION_ARGS);
extern Datum gin_consistent_jsonb_path(PG_FUNCTION_ARGS);
extern Datum gin_triconsistent_jsonb_path(PG_FUNCTION_ARGS);

/* Support functions */
extern int	compareJsonbContainers(JsonbContainer *a, JsonbContainer *b);
extern JsonbValue *findJsonbValueFromContainer(JsonbContainer *sheader,
							uint32 flags,
							JsonbValue *key);
extern JsonbValue *getIthJsonbValueFromContainer(JsonbContainer *sheader,
							  uint32 i);
extern JsonbValue *pushJsonbValue(JsonbParseState **pstate,
			   JsonbIteratorToken seq, JsonbValue *scalarVal);
extern JsonbIterator *JsonbIteratorInit(JsonbContainer *container);
extern JsonbIteratorToken JsonbIteratorNext(JsonbIterator **it, JsonbValue *val,
				  bool skipNested);
extern Jsonb *JsonbValueToJsonb(JsonbValue *val);
extern bool JsonbDeepContains(JsonbIterator **val,
				  JsonbIterator **mContained);
extern void JsonbHashScalarValue(const JsonbValue *scalarVal, uint32 *hash);

/* jsonb.c support function */
extern char *JsonbToCString(StringInfo out, JsonbContainer *in,
			   int estimated_len);

#endif   /* __JSONB_H__ */
