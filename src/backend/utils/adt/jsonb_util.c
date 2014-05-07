/*-------------------------------------------------------------------------
 *
 * jsonb_util.c
 *	  converting between Jsonb and JsonbValues, and iterating.
 *
 * Copyright (c) 2014, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb_util.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "catalog/pg_collation.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"

/*
 * Twice as many values may be stored within pairs (for an Object) than within
 * elements (for an Array), modulo the current MaxAllocSize limitation.  Note
 * that JSONB_MAX_PAIRS is derived from the number of possible pairs, not
 * values (as is the case for arrays and their elements), because we're
 * concerned about limitations on the representation of the number of pairs.
 * Over twice the memory is required to store n JsonbPairs as n JsonbValues.
 * It only takes exactly twice as much disk space for storage, though.  The
 * JsonbPair (not an actual pair of values) representation is used here because
 * that is what is subject to the MaxAllocSize restriction when building an
 * object.
 */
#define JSONB_MAX_ELEMS (Min(MaxAllocSize / sizeof(JsonbValue), JENTRY_POSMASK))
#define JSONB_MAX_PAIRS (Min(MaxAllocSize / sizeof(JsonbPair), \
							 JENTRY_POSMASK))

/*
 * convertState: a resizeable buffer used when constructing a Jsonb datum
 */
typedef struct
{
	char	   *buffer;
	int			len;
	int			allocatedsz;
} convertState;

static void fillJsonbValue(JEntry *entry, char *payload_base, JsonbValue *result);
static int	compareJsonbScalarValue(JsonbValue *a, JsonbValue *b);
static int	lexicalCompareJsonbStringValue(const void *a, const void *b);
static Jsonb *convertToJsonb(JsonbValue *val);
static void convertJsonbValue(convertState *buffer, JEntry *header, JsonbValue *val, int level);
static void convertJsonbArray(convertState *buffer, JEntry *header, JsonbValue *val, int level);
static void convertJsonbObject(convertState *buffer, JEntry *header, JsonbValue *val, int level);
static void convertJsonbScalar(convertState *buffer, JEntry *header, JsonbValue *scalarVal);

static int reserveFromBuffer(convertState *buffer, int len);
static void appendToBuffer(convertState *buffer, char *data, int len);
static void copyToBuffer(convertState *buffer, int offset, char *data, int len);
static short padBufferToInt(convertState *buffer);

static void iteratorFromContainer(JsonbIterator *it, JsonbContainer *container);
static bool formIterIsContainer(JsonbIterator **it, JsonbValue *val,
					JEntry *ent, bool skipNested);
static JsonbIterator *freeAndGetParent(JsonbIterator *it);
static JsonbParseState *pushState(JsonbParseState **pstate);
static void appendKey(JsonbParseState *pstate, JsonbValue *scalarVal);
static void appendValue(JsonbParseState *pstate, JsonbValue *scalarVal);
static void appendElement(JsonbParseState *pstate, JsonbValue *scalarVal);
static int	lengthCompareJsonbStringValue(const void *a, const void *b, void *arg);
static int	lengthCompareJsonbPair(const void *a, const void *b, void *arg);
static void uniqueifyJsonbObject(JsonbValue *object);

/*
 * Turn an in-memory JsonbValue into a Jsonb for on-disk storage.
 *
 * There isn't a JsonbToJsonbValue(), because generally we find it more
 * convenient to directly iterate through the Jsonb representation and only
 * really convert nested scalar values.  formIterIsContainer() does this, so
 * that clients of the iteration code don't have to directly deal with the
 * binary representation (JsonbDeepContains() is a notable exception, although
 * all exceptions are internal to this module).  In general, functions that
 * accept a JsonbValue argument are concerned with the manipulation of scalar
 * values, or simple containers of scalar values, where it would be
 * inconvenient to deal with a great amount of other state.
 */
Jsonb *
JsonbValueToJsonb(JsonbValue *val)
{
	Jsonb	   *out;

	if (IsAJsonbScalar(val))
	{
		/* Scalar value */
		JsonbParseState *pstate = NULL;
		JsonbValue *res;
		JsonbValue	scalarArray;

		scalarArray.type = jbvArray;
		scalarArray.val.array.rawScalar = true;
		scalarArray.val.array.nElems = 1;

		pushJsonbValue(&pstate, WJB_BEGIN_ARRAY, &scalarArray);
		pushJsonbValue(&pstate, WJB_ELEM, val);
		res = pushJsonbValue(&pstate, WJB_END_ARRAY, NULL);

		out = convertToJsonb(res);
	}
	else if (val->type == jbvObject || val->type == jbvArray)
	{
		out = convertToJsonb(val);
	}
	else
	{
		Assert(val->type == jbvBinary);
		out = palloc(VARHDRSZ + val->val.binary.len);
		SET_VARSIZE(out, VARHDRSZ + val->val.binary.len);
		memcpy(VARDATA(out), val->val.binary.data, val->val.binary.len);
	}

	return out;
}

/*
 * BT comparator worker function.  Returns an integer less than, equal to, or
 * greater than zero, indicating whether a is less than, equal to, or greater
 * than b.  Consistent with the requirements for a B-Tree operator class
 *
 * Strings are compared lexically, in contrast with other places where we use a
 * much simpler comparator logic for searching through Strings.  Since this is
 * called from B-Tree support function 1, we're careful about not leaking
 * memory here.
 */
int
compareJsonbContainers(JsonbContainer *a, JsonbContainer *b)
{
	JsonbIterator *ita,
			   *itb;
	int			res = 0;

	ita = JsonbIteratorInit(a);
	itb = JsonbIteratorInit(b);

	do
	{
		JsonbValue	va,
					vb;
		int			ra,
					rb;

		ra = JsonbIteratorNext(&ita, &va, false);
		rb = JsonbIteratorNext(&itb, &vb, false);

		/*
		 * To a limited extent we'll redundantly iterate over an array/object
		 * while re-performing the same test without any reasonable
		 * expectation of the same container types having differing lengths
		 * (as when we process a WJB_BEGIN_OBJECT, and later the corresponding
		 * WJB_END_OBJECT), but no matter.
		 */
		if (ra == rb)
		{
			if (ra == WJB_DONE)
			{
				/* Decisively equal */
				break;
			}

			if (va.type == vb.type)
			{
				switch (va.type)
				{
					case jbvString:
						res = lexicalCompareJsonbStringValue(&va, &vb);
						break;
					case jbvNull:
					case jbvNumeric:
					case jbvBool:
						res = compareJsonbScalarValue(&va, &vb);
						break;
					case jbvArray:

						/*
						 * This could be a "raw scalar" pseudo array.  That's
						 * a special case here though, since we still want the
						 * general type-based comparisons to apply, and as far
						 * as we're concerned a pseudo array is just a scalar.
						 */
						if (va.val.array.rawScalar != vb.val.array.rawScalar)
							res = (va.val.array.rawScalar) ? -1 : 1;
						if (va.val.array.nElems != vb.val.array.nElems)
							res = (va.val.array.nElems > vb.val.array.nElems) ? 1 : -1;
						break;
					case jbvObject:
						if (va.val.object.nPairs != vb.val.object.nPairs)
							res = (va.val.object.nPairs > vb.val.object.nPairs) ? 1 : -1;
						break;
					case jbvBinary:
						elog(ERROR, "unexpected jbvBinary value");
				}
			}
			else
			{
				/* Type-defined order */
				res = (va.type > vb.type) ? 1 : -1;
			}
		}
		else
		{
			/*
			 * It's safe to assume that the types differed.
			 *
			 * If the two values were the same container type, then there'd
			 * have been a chance to observe the variation in the number of
			 * elements/pairs (when processing WJB_BEGIN_OBJECT, say).  They
			 * can't be scalar types either, because then they'd have to be
			 * contained in containers already ruled unequal due to differing
			 * numbers of pairs/elements, or already directly ruled unequal
			 * with a call to the underlying type's comparator.
			 */
			Assert(va.type != vb.type);
			Assert(va.type == jbvArray || va.type == jbvObject);
			Assert(vb.type == jbvArray || vb.type == jbvObject);
			/* Type-defined order */
			res = (va.type > vb.type) ? 1 : -1;
		}
	}
	while (res == 0);

	while (ita != NULL)
	{
		JsonbIterator *i = ita->parent;

		pfree(ita);
		ita = i;
	}
	while (itb != NULL)
	{
		JsonbIterator *i = itb->parent;

		pfree(itb);
		itb = i;
	}

	return res;
}

/*
 * Find value in object (i.e. the "value" part of some key/value pair in an
 * object), or find a matching element if we're looking through an array.  Do
 * so on the basis of equality of the object keys only, or alternatively
 * element values only, with a caller-supplied value "key".  The "flags"
 * argument allows the caller to specify which container types are of interest.
 *
 * This exported utility function exists to facilitate various cases concerned
 * with "containment".  If asked to look through an object, the caller had
 * better pass a Jsonb String, because their keys can only be strings.
 * Otherwise, for an array, any type of JsonbValue will do.
 *
 * In order to proceed with the search, it is necessary for callers to have
 * both specified an interest in exactly one particular container type with an
 * appropriate flag, as well as having the pointed-to Jsonb container be of
 * one of those same container types at the top level. (Actually, we just do
 * whichever makes sense to save callers the trouble of figuring it out - at
 * most one can make sense, because the container either points to an array
 * (possibly a "raw scalar" pseudo array) or an object.)
 *
 * Note that we can return a jbvBinary JsonbValue if this is called on an
 * object, but we never do so on an array.  If the caller asks to look through
 * a container type that is not of the type pointed to by the container,
 * immediately fall through and return NULL.  If we cannot find the value,
 * return NULL.  Otherwise, return palloc()'d copy of value.
 */
JsonbValue *
findJsonbValueFromContainer(JsonbContainer *container, uint32 flags,
							JsonbValue *key)
{
	JEntry	   *array = container->children;
	int			count = (container->header & JB_CMASK);
	JsonbValue *result = palloc(sizeof(JsonbValue));

	Assert((flags & ~(JB_FARRAY | JB_FOBJECT)) == 0);

	if (flags & JB_FARRAY & container->header)
	{
		char	   *data = (char *) (array + (container->header & JB_CMASK));
		int			i;

		for (i = 0; i < count; i++)
		{
			JEntry	   *e = array + i;

			fillJsonbValue(e, data, result);

			if (key->type == result->type)
			{
				if (compareJsonbScalarValue(key, result) == 0)
					return result;
			}
		}
	}
	else if (flags & JB_FOBJECT & container->header)
	{
		/* Since this is an object, account for *Pairs* of Jentrys */
		char	   *data = (char *) (array + (container->header & JB_CMASK) * 2);
		uint32		stopLow = 0,
					stopMiddle;

		/* Object key past by caller must be a string */
		Assert(key->type == jbvString);

		/* Binary search on object/pair keys *only* */
		while (stopLow < count)
		{
			JEntry	   *entry;
			int			difference;
			JsonbValue	candidate;

			/*
			 * Note how we compensate for the fact that we're iterating
			 * through pairs (not entries) throughout.
			 */
			stopMiddle = stopLow + (count - stopLow) / 2;

			entry = array + stopMiddle * 2;

			candidate.type = jbvString;
			candidate.val.string.val = data + JBE_OFF(*entry);
			candidate.val.string.len = JBE_LEN(*entry);

			difference = lengthCompareJsonbStringValue(&candidate, key, NULL);

			if (difference == 0)
			{
				/* Found our value (from key/value pair) */
				JEntry	   *v = entry + 1;

				fillJsonbValue(v, data, result);

				return result;
			}
			else
			{
				if (difference < 0)
					stopLow = stopMiddle + 1;
				else
					count = stopMiddle;
			}
		}
	}

	/* Not found */
	pfree(result);
	return NULL;
}

/*
 * Get i-th value of a Jsonb array.
 *
 * Returns palloc()'d copy of the value, or NULL if it does not exist.
 */
JsonbValue *
getIthJsonbValueFromContainer(JsonbContainer *container, uint32 i)
{
	JsonbValue *result;
	JEntry	   *e;
	char	   *data;
	uint32		nelements;

	if ((container->header & JB_FARRAY) == 0)
		elog(ERROR, "not a jsonb array");

	nelements = container->header & JB_CMASK;

	if (i >= nelements)
		return NULL;

	e = &container->children[i];

	data = (char *) &container->children[nelements];

	result = palloc(sizeof(JsonbValue));

	fillJsonbValue(e, data, result);

	return result;
}

/*
 * Given the JEntry header, and the base address of the data that the offset
 * in the JEntry refers to, fill a JsonbValue.
 *
 * An array or object will be returned as jbvBinary, ie. it won't be
 * expanded.
 */
static void
fillJsonbValue(JEntry *entry, char *payload_base, JsonbValue *result)
{
	if (JBE_ISNULL(*entry))
	{
		result->type = jbvNull;
	}
	else if (JBE_ISSTRING(*entry))
	{
		result->type = jbvString;
		result->val.string.val = payload_base + JBE_OFF(*entry);
		result->val.string.len = JBE_LEN(*entry);
		Assert(result->val.string.len >= 0);
	}
	else if (JBE_ISNUMERIC(*entry))
	{
		result->type = jbvNumeric;
		result->val.numeric = (Numeric) (payload_base + INTALIGN(JBE_OFF(*entry)));
	}
	else if (JBE_ISBOOL_TRUE(*entry))
	{
		result->type = jbvBool;
		result->val.boolean = true;
	}
	else if (JBE_ISBOOL_FALSE(*entry))
	{
		result->type = jbvBool;
		result->val.boolean = false;
	}
	else
	{
		Assert(JBE_ISCONTAINER(*entry));
		result->type = jbvBinary;
		result->val.binary.data = (JsonbContainer *) (payload_base + INTALIGN(JBE_OFF(*entry)));
		result->val.binary.len = JBE_LEN(*entry) - (INTALIGN(JBE_OFF(*entry)) - JBE_OFF(*entry));
	}
}

/*
 * Push JsonbValue into JsonbParseState.
 *
 * Used when parsing JSON tokens to form Jsonb, or when converting an in-memory
 * JsonbValue to a Jsonb.
 *
 * Initial state of *JsonbParseState is NULL, since it'll be allocated here
 * originally (caller will get JsonbParseState back by reference).
 *
 * Only sequential tokens pertaining to non-container types should pass a
 * JsonbValue.  There is one exception -- WJB_BEGIN_ARRAY callers may pass a
 * "raw scalar" pseudo array to append that.
 */
JsonbValue *
pushJsonbValue(JsonbParseState **pstate, JsonbIteratorToken seq,
			   JsonbValue *scalarVal)
{
	JsonbValue *result = NULL;

	switch (seq)
	{
		case WJB_BEGIN_ARRAY:
			Assert(!scalarVal || scalarVal->val.array.rawScalar);
			*pstate = pushState(pstate);
			result = &(*pstate)->contVal;
			(*pstate)->contVal.type = jbvArray;
			(*pstate)->contVal.val.array.nElems = 0;
			(*pstate)->contVal.val.array.rawScalar = (scalarVal &&
											 scalarVal->val.array.rawScalar);
			if (scalarVal && scalarVal->val.array.nElems > 0)
			{
				/* Assume that this array is still really a scalar */
				Assert(scalarVal->type == jbvArray);
				(*pstate)->size = scalarVal->val.array.nElems;
			}
			else
			{
				(*pstate)->size = 4;
			}
			(*pstate)->contVal.val.array.elems = palloc(sizeof(JsonbValue) *
														(*pstate)->size);
			break;
		case WJB_BEGIN_OBJECT:
			Assert(!scalarVal);
			*pstate = pushState(pstate);
			result = &(*pstate)->contVal;
			(*pstate)->contVal.type = jbvObject;
			(*pstate)->contVal.val.object.nPairs = 0;
			(*pstate)->size = 4;
			(*pstate)->contVal.val.object.pairs = palloc(sizeof(JsonbPair) *
														 (*pstate)->size);
			break;
		case WJB_KEY:
			Assert(scalarVal->type == jbvString);
			appendKey(*pstate, scalarVal);
			break;
		case WJB_VALUE:
			Assert(IsAJsonbScalar(scalarVal) ||
				   scalarVal->type == jbvBinary);
			appendValue(*pstate, scalarVal);
			break;
		case WJB_ELEM:
			Assert(IsAJsonbScalar(scalarVal) ||
				   scalarVal->type == jbvBinary);
			appendElement(*pstate, scalarVal);
			break;
		case WJB_END_OBJECT:
			uniqueifyJsonbObject(&(*pstate)->contVal);
			/* fall through! */
		case WJB_END_ARRAY:
			/* Steps here common to WJB_END_OBJECT case */
			Assert(!scalarVal);
			result = &(*pstate)->contVal;

			/*
			 * Pop stack and push current array/object as value in parent
			 * array/object
			 */
			*pstate = (*pstate)->next;
			if (*pstate)
			{
				switch ((*pstate)->contVal.type)
				{
					case jbvArray:
						appendElement(*pstate, result);
						break;
					case jbvObject:
						appendValue(*pstate, result);
						break;
					default:
						elog(ERROR, "invalid jsonb container type");
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized jsonb sequential processing token");
	}

	return result;
}

/*
 * Given a JsonbContainer, expand to JsonbIterator to iterate over items
 * fully expanded to in-memory representation for manipulation.
 *
 * See JsonbIteratorNext() for notes on memory management.
 */
JsonbIterator *
JsonbIteratorInit(JsonbContainer *container)
{
	JsonbIterator *it = palloc(sizeof(JsonbIterator));

	iteratorFromContainer(it, container);
	it->parent = NULL;

	return it;
}

/*
 * Get next JsonbValue while iterating
 *
 * Caller should initially pass their own, original iterator.  They may get
 * back a child iterator palloc()'d here instead.  The function can be relied
 * on to free those child iterators, lest the memory allocated for highly
 * nested objects become unreasonable, but only if callers don't end iteration
 * early (by breaking upon having found something in a search, for example).
 *
 * Callers in such a scenario, that are particularly sensitive to leaking
 * memory in a long-lived context may walk the ancestral tree from the final
 * iterator we left them with to its oldest ancestor, pfree()ing as they go.
 * They do not have to free any other memory previously allocated for iterators
 * but not accessible as direct ancestors of the iterator they're last passed
 * back.
 *
 * Returns "Jsonb sequential processing" token value.  Iterator "state"
 * reflects the current stage of the process in a less granular fashion, and is
 * mostly used here to track things internally with respect to particular
 * iterators.
 *
 * Clients of this function should not have to handle any jbvBinary values
 * (since recursive calls will deal with this), provided skipNested is false.
 * It is our job to expand the jbvBinary representation without bothering them
 * with it.  However, clients should not take it upon themselves to touch array
 * or Object element/pair buffers, since their element/pair pointers are
 * garbage.
 */
JsonbIteratorToken
JsonbIteratorNext(JsonbIterator **it, JsonbValue *val, bool skipNested)
{
	JsonbIterState state;

	/* Guard against stack overflow due to overly complex Jsonb */
	check_stack_depth();

	/* Recursive caller may have original caller's iterator */
	if (*it == NULL)
		return WJB_DONE;

	state = (*it)->state;

	if ((*it)->containerType == JB_FARRAY)
	{
		if (state == jbi_start)
		{
			/* Set v to array on first array call */
			val->type = jbvArray;
			val->val.array.nElems = (*it)->nElems;

			/*
			 * v->val.array.elems is not actually set, because we aren't doing
			 * a full conversion
			 */
			val->val.array.rawScalar = (*it)->isScalar;
			(*it)->i = 0;
			/* Set state for next call */
			(*it)->state = jbi_elem;
			return WJB_BEGIN_ARRAY;
		}
		else if (state == jbi_elem)
		{
			if ((*it)->i >= (*it)->nElems)
			{
				/*
				 * All elements within array already processed.  Report this
				 * to caller, and give it back original parent iterator (which
				 * independently tracks iteration progress at its level of
				 * nesting).
				 */
				*it = freeAndGetParent(*it);
				return WJB_END_ARRAY;
			}
			else if (formIterIsContainer(it, val, &(*it)->meta[(*it)->i++],
										 skipNested))
			{
				/*
				 * New child iterator acquired within formIterIsContainer.
				 * Recurse into container.  Don't directly return jbvBinary
				 * value to top-level client.
				 */
				return JsonbIteratorNext(it, val, skipNested);
			}
			else
			{
				/* Scalar item in array */
				return WJB_ELEM;
			}
		}
	}
	else if ((*it)->containerType == JB_FOBJECT)
	{
		if (state == jbi_start)
		{
			/* Set v to object on first object call */
			val->type = jbvObject;
			val->val.object.nPairs = (*it)->nElems;

			/*
			 * v->val.object.pairs is not actually set, because we aren't
			 * doing a full conversion
			 */
			(*it)->i = 0;
			/* Set state for next call */
			(*it)->state = jbi_key;
			return WJB_BEGIN_OBJECT;
		}
		else if (state == jbi_key)
		{
			if ((*it)->i >= (*it)->nElems)
			{
				/*
				 * All pairs within object already processed.  Report this to
				 * caller, and give it back original containing iterator
				 * (which independently tracks iteration progress at its level
				 * of nesting).
				 */
				*it = freeAndGetParent(*it);
				return WJB_END_OBJECT;
			}
			else
			{
				/*
				 * Return binary item key (ensured by setting skipNested to
				 * false directly).  No child iterator, no further recursion.
				 * When control reaches here, it's probably from a recursive
				 * call.
				 */
				if (formIterIsContainer(it, val, &(*it)->meta[(*it)->i * 2], false))
					elog(ERROR, "unexpected container as object key");

				Assert(val->type == jbvString);
				/* Set state for next call */
				(*it)->state = jbi_value;
				return WJB_KEY;
			}
		}
		else if (state == jbi_value)
		{
			/* Set state for next call */
			(*it)->state = jbi_key;

			/*
			 * Value may be a container, in which case we recurse with new,
			 * child iterator.  If it is, don't bother !skipNested callers
			 * with dealing with the jbvBinary representation.
			 */
			if (formIterIsContainer(it, val, &(*it)->meta[((*it)->i++) * 2 + 1],
									skipNested))
				return JsonbIteratorNext(it, val, skipNested);
			else
				return WJB_VALUE;
		}
	}

	elog(ERROR, "invalid iterator state");
	return -1;
}

/*
 * Worker for "contains" operator's function
 *
 * Formally speaking, containment is top-down, unordered subtree isomorphism.
 *
 * Takes iterators that belong to some container type.  These iterators
 * "belong" to those values in the sense that they've just been initialized in
 * respect of them by the caller (perhaps in a nested fashion).
 *
 * "val" is lhs Jsonb, and mContained is rhs Jsonb when called from top level.
 * We determine if mContained is contained within val.
 */
bool
JsonbDeepContains(JsonbIterator **val, JsonbIterator **mContained)
{
	uint32		rval,
				rcont;
	JsonbValue	vval,
				vcontained;

	/*
	 * Guard against stack overflow due to overly complex Jsonb.
	 *
	 * Functions called here independently take this precaution, but that
	 * might not be sufficient since this is also a recursive function.
	 */
	check_stack_depth();

	rval = JsonbIteratorNext(val, &vval, false);
	rcont = JsonbIteratorNext(mContained, &vcontained, false);

	if (rval != rcont)
	{
		/*
		 * The differing return values can immediately be taken as indicating
		 * two differing container types at this nesting level, which is
		 * sufficient reason to give up entirely (but it should be the case
		 * that they're both some container type).
		 */
		Assert(rval == WJB_BEGIN_OBJECT || rval == WJB_BEGIN_ARRAY);
		Assert(rcont == WJB_BEGIN_OBJECT || rcont == WJB_BEGIN_ARRAY);
		return false;
	}
	else if (rcont == WJB_BEGIN_OBJECT)
	{
		JsonbValue *lhsVal;		/* lhsVal is from pair in lhs object */

		Assert(vcontained.type == jbvObject);

		/* Work through rhs "is it contained within?" object */
		for (;;)
		{
			rcont = JsonbIteratorNext(mContained, &vcontained, false);

			/*
			 * When we get through caller's rhs "is it contained within?"
			 * object without failing to find one of its values, it's
			 * contained.
			 */
			if (rcont == WJB_END_OBJECT)
				return true;

			Assert(rcont == WJB_KEY);

			/* First, find value by key... */
			lhsVal = findJsonbValueFromContainer((JsonbContainer *) (*val)->buffer,
												 JB_FOBJECT,
												 &vcontained);

			if (!lhsVal)
				return false;

			/*
			 * ...at this stage it is apparent that there is at least a key
			 * match for this rhs pair.
			 */
			rcont = JsonbIteratorNext(mContained, &vcontained, true);

			Assert(rcont == WJB_VALUE);

			/*
			 * Compare rhs pair's value with lhs pair's value just found using
			 * key
			 */
			if (lhsVal->type != vcontained.type)
			{
				return false;
			}
			else if (IsAJsonbScalar(lhsVal))
			{
				if (compareJsonbScalarValue(lhsVal, &vcontained) != 0)
					return false;
			}
			else
			{
				/* Nested container value (object or array) */
				JsonbIterator *nestval,
						   *nestContained;

				Assert(lhsVal->type == jbvBinary);
				Assert(vcontained.type == jbvBinary);

				nestval = JsonbIteratorInit(lhsVal->val.binary.data);
				nestContained = JsonbIteratorInit(vcontained.val.binary.data);

				/*
				 * Match "value" side of rhs datum object's pair recursively.
				 * It's a nested structure.
				 *
				 * Note that nesting still has to "match up" at the right
				 * nesting sub-levels.  However, there need only be zero or
				 * more matching pairs (or elements) at each nesting level
				 * (provided the *rhs* pairs/elements *all* match on each
				 * level), which enables searching nested structures for a
				 * single String or other primitive type sub-datum quite
				 * effectively (provided the user constructed the rhs nested
				 * structure such that we "know where to look").
				 *
				 * In other words, the mapping of container nodes in the rhs
				 * "vcontained" Jsonb to internal nodes on the lhs is
				 * injective, and parent-child edges on the rhs must be mapped
				 * to parent-child edges on the lhs to satisfy the condition
				 * of containment (plus of course the mapped nodes must be
				 * equal).
				 */
				if (!JsonbDeepContains(&nestval, &nestContained))
					return false;
			}
		}
	}
	else if (rcont == WJB_BEGIN_ARRAY)
	{
		JsonbValue *lhsConts = NULL;
		uint32		nLhsElems = vval.val.array.nElems;

		Assert(vcontained.type == jbvArray);

		/*
		 * Handle distinction between "raw scalar" pseudo arrays, and real
		 * arrays.
		 *
		 * A raw scalar may contain another raw scalar, and an array may
		 * contain a raw scalar, but a raw scalar may not contain an array. We
		 * don't do something like this for the object case, since objects can
		 * only contain pairs, never raw scalars (a pair is represented by an
		 * rhs object argument with a single contained pair).
		 */
		if (vval.val.array.rawScalar && !vcontained.val.array.rawScalar)
			return false;

		/* Work through rhs "is it contained within?" array */
		for (;;)
		{
			rcont = JsonbIteratorNext(mContained, &vcontained, true);

			/*
			 * When we get through caller's rhs "is it contained within?"
			 * array without failing to find one of its values, it's
			 * contained.
			 */
			if (rcont == WJB_END_ARRAY)
				return true;

			Assert(rcont == WJB_ELEM);

			if (IsAJsonbScalar(&vcontained))
			{
				if (!findJsonbValueFromContainer((JsonbContainer *) (*val)->buffer,
												 JB_FARRAY,
												 &vcontained))
					return false;
			}
			else
			{
				uint32		i;

				/*
				 * If this is first container found in rhs array (at this
				 * depth), initialize temp lhs array of containers
				 */
				if (lhsConts == NULL)
				{
					uint32		j = 0;

					/* Make room for all possible values */
					lhsConts = palloc(sizeof(JsonbValue) * nLhsElems);

					for (i = 0; i < nLhsElems; i++)
					{
						/* Store all lhs elements in temp array */
						rcont = JsonbIteratorNext(val, &vval, true);
						Assert(rcont == WJB_ELEM);

						if (vval.type == jbvBinary)
							lhsConts[j++] = vval;
					}

					/* No container elements in temp array, so give up now */
					if (j == 0)
						return false;

					/* We may have only partially filled array */
					nLhsElems = j;
				}

				/* XXX: Nested array containment is O(N^2) */
				for (i = 0; i < nLhsElems; i++)
				{
					/* Nested container value (object or array) */
					JsonbIterator *nestval,
							   *nestContained;
					bool		contains;

					nestval = JsonbIteratorInit(lhsConts[i].val.binary.data);
					nestContained = JsonbIteratorInit(vcontained.val.binary.data);

					contains = JsonbDeepContains(&nestval, &nestContained);

					if (nestval)
						pfree(nestval);
					if (nestContained)
						pfree(nestContained);
					if (contains)
						break;
				}

				/*
				 * Report rhs container value is not contained if couldn't
				 * match rhs container to *some* lhs cont
				 */
				if (i == nLhsElems)
					return false;
			}
		}
	}
	else
	{
		elog(ERROR, "invalid jsonb container type");
	}

	elog(ERROR, "unexpectedly fell off end of jsonb container");
	return false;
}

/*
 * Hash a JsonbValue scalar value, mixing the hash value into an existing
 * hash provided by the caller.
 *
 * Some callers may wish to independently XOR in JB_FOBJECT and JB_FARRAY
 * flags.
 */
void
JsonbHashScalarValue(const JsonbValue *scalarVal, uint32 *hash)
{
	uint32		tmp;

	/* Compute hash value for scalarVal */
	switch (scalarVal->type)
	{
		case jbvNull:
			tmp = 0x01;
			break;
		case jbvString:
			tmp = DatumGetUInt32(hash_any((const unsigned char *) scalarVal->val.string.val,
										  scalarVal->val.string.len));
			break;
		case jbvNumeric:
			/* Must hash equal numerics to equal hash codes */
			tmp = DatumGetUInt32(DirectFunctionCall1(hash_numeric,
								   NumericGetDatum(scalarVal->val.numeric)));
			break;
		case jbvBool:
			tmp = scalarVal->val.boolean ? 0x02 : 0x04;
			break;
		default:
			elog(ERROR, "invalid jsonb scalar type");
			tmp = 0;			/* keep compiler quiet */
			break;
	}

	/*
	 * Combine hash values of successive keys, values and elements by rotating
	 * the previous value left 1 bit, then XOR'ing in the new
	 * key/value/element's hash value.
	 */
	*hash = (*hash << 1) | (*hash >> 31);
	*hash ^= tmp;
}

/*
 * Are two scalar JsonbValues of the same type a and b equal?
 *
 * Does not use lexical comparisons.  Therefore, it is essentially that this
 * never be used against Strings for anything other than searching for values
 * within a single jsonb.
 */
static int
compareJsonbScalarValue(JsonbValue *aScalar, JsonbValue *bScalar)
{
	if (aScalar->type == bScalar->type)
	{
		switch (aScalar->type)
		{
			case jbvNull:
				return 0;
			case jbvString:
				return lengthCompareJsonbStringValue(aScalar, bScalar, NULL);
			case jbvNumeric:
				return DatumGetInt32(DirectFunctionCall2(numeric_cmp,
									   PointerGetDatum(aScalar->val.numeric),
									 PointerGetDatum(bScalar->val.numeric)));
			case jbvBool:
				if (aScalar->val.boolean != bScalar->val.boolean)
					return (aScalar->val.boolean > bScalar->val.boolean) ? 1 : -1;
				else
					return 0;
			default:
				elog(ERROR, "invalid jsonb scalar type");
		}
	}
	elog(ERROR, "jsonb scalar type mismatch");
	return -1;
}

/*
 * Standard lexical qsort() comparator of jsonb strings.
 *
 * Sorts strings lexically, using the default database collation.  Used by
 * B-Tree operators, where a lexical sort order is generally expected.
 */
static int
lexicalCompareJsonbStringValue(const void *a, const void *b)
{
	const JsonbValue *va = (const JsonbValue *) a;
	const JsonbValue *vb = (const JsonbValue *) b;

	Assert(va->type == jbvString);
	Assert(vb->type == jbvString);

	return varstr_cmp(va->val.string.val, va->val.string.len, vb->val.string.val,
					  vb->val.string.len, DEFAULT_COLLATION_OID);
}


/*
 * Functions for manipulating the resizeable buffer used by convertJsonb and
 * its subroutines.
 */

/*
 * Rervere 'len' bytes, at the end of the buffer, enlarging it if necessary.
 * Returns the offset to the reserved area. The caller is expected to copy
 * the data to the reserved area later with copyToBuffer()
 */
static int
reserveFromBuffer(convertState *buffer, int len)
{
	int			offset;

	/* Make more room if needed */
	if (buffer->len + len > buffer->allocatedsz)
	{
		buffer->allocatedsz *= 2;
		buffer->buffer = repalloc(buffer->buffer, buffer->allocatedsz);
	}

	/* remember current offset */
	offset = buffer->len;

	/* reserve the space */
	buffer->len += len;

	return offset;
}

/*
 * Copy 'len' bytes to a previously reserved area in buffer.
 */
static void
copyToBuffer(convertState *buffer, int offset, char *data, int len)
{
	memcpy(buffer->buffer + offset, data, len);
}

/*
 * A shorthand for reserveFromBuffer + copyToBuffer.
 */
static void
appendToBuffer(convertState *buffer, char *data, int len)
{
	int			offset;

	offset = reserveFromBuffer(buffer, len);
	copyToBuffer(buffer, offset, data, len);
}


/*
 * Append padding, so that the length of the StringInfo is int-aligned.
 * Returns the number of padding bytes appended.
 */
static short
padBufferToInt(convertState *buffer)
{
	short		padlen,
				p;
	int			offset;

	padlen = INTALIGN(buffer->len) - buffer->len;

	offset = reserveFromBuffer(buffer, padlen);
	for (p = 0; p < padlen; p++)
		buffer->buffer[offset + p] = 0;

	return padlen;
}

/*
 * Given a JsonbValue, convert to Jsonb. The result is palloc'd.
 */
static Jsonb *
convertToJsonb(JsonbValue *val)
{
	convertState buffer;
	JEntry		jentry;
	Jsonb	   *res;

	/* Should not already have binary representation */
	Assert(val->type != jbvBinary);

	/* Allocate an output buffer. It will be enlarged as needed */
	buffer.buffer = palloc(128);
	buffer.len = 0;
	buffer.allocatedsz = 128;

	/* Make room for the varlena header */
	reserveFromBuffer(&buffer, sizeof(VARHDRSZ));

	convertJsonbValue(&buffer, &jentry, val, 0);

	/*
	 * Note: the JEntry of the root is not discarded. Therefore the root
	 * JsonbContainer struct must contain enough information to tell what
	 * kind of value it is.
	 */

	res = (Jsonb *) buffer.buffer;

	SET_VARSIZE(res, buffer.len);

	return res;
}

/*
 * Subroutine of convertJsonb: serialize a single JsonbValue into buffer.
 *
 * The JEntry header for this node is returned in *header. It is filled in
 * with the length of this value, but if
 * it is stored in an array or an object (which is always, except for the root
 * node), it is the caller's responsibility to adjust it with the offset
 * within the container.
 *
 * If the value is an array or an object, this recurses. 'level' is only used
 * for debugging purposes.

 * As part of the process of converting an arbitrary JsonbValue to a Jsonb,
 * serialize and copy a scalar value into buffer.
 *
 * This is a worker function for putJsonbValueConversion() (itself a worker for
 * walkJsonbValueConversion()).  It handles the details with regard to Jentry
 * metadata peculiar to each scalar type.
 *
 * It is the callers responsibility to shift the offset if this is stored
 * in an array or object.
 */
static void
convertJsonbValue(convertState *buffer, JEntry *header, JsonbValue *val, int level)
{
	check_stack_depth();

	if (!val)
		return;

	if (IsAJsonbScalar(val) || val->type == jbvBinary)
		convertJsonbScalar(buffer, header, val);
	else if (val->type == jbvArray)
		convertJsonbArray(buffer, header, val, level);
	else if (val->type == jbvObject)
		convertJsonbObject(buffer, header, val, level);
	else
		elog(ERROR, "unknown type of jsonb container");
}

static void
convertJsonbArray(convertState *buffer, JEntry *pheader, JsonbValue *val, int level)
{
	int			offset;
	int			metaoffset;
	int			i;
	int			totallen;
	uint32		header;

	/* Initialize pointer into conversion buffer at this level */
	offset = buffer->len;

	padBufferToInt(buffer);

	/*
	 * Construct the header Jentry, stored in the beginning of the variable-
	 * length payload.
	 */
	header = val->val.array.nElems | JB_FARRAY;
	if (val->val.array.rawScalar)
	{
		Assert(val->val.array.nElems == 1);
		Assert(level == 0);
		header |= JB_FSCALAR;
	}

	appendToBuffer(buffer, (char *) &header, sizeof(uint32));
	/* reserve space for the JEntries of the elements. */
	metaoffset = reserveFromBuffer(buffer, sizeof(JEntry) * val->val.array.nElems);

	totallen = 0;
	for (i = 0; i < val->val.array.nElems; i++)
	{
		JsonbValue *elem = &val->val.array.elems[i];
		int			len;
		JEntry		meta;

		convertJsonbValue(buffer, &meta, elem, level + 1);
		len = meta & JENTRY_POSMASK;
		totallen += len;

		if (totallen > JENTRY_POSMASK)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("total size of jsonb array elements exceeds the maximum of %u bytes",
							JENTRY_POSMASK)));

		if (i == 0)
			meta |= JENTRY_ISFIRST;
		else
			meta = (meta & ~JENTRY_POSMASK) | totallen;
		copyToBuffer(buffer, metaoffset, (char *) &meta, sizeof(JEntry));
		metaoffset += sizeof(JEntry);
	}

	totallen = buffer->len - offset;

	/* Initialize the header of this node, in the container's JEntry array */
	*pheader = JENTRY_ISCONTAINER | totallen;
}

static void
convertJsonbObject(convertState *buffer, JEntry *pheader, JsonbValue *val, int level)
{
	uint32		header;
	int			offset;
	int			metaoffset;
	int			i;
	int			totallen;

	/* Initialize pointer into conversion buffer at this level */
	offset = buffer->len;

	padBufferToInt(buffer);

	/* Initialize header */
	header = val->val.object.nPairs | JB_FOBJECT;
	appendToBuffer(buffer, (char *) &header, sizeof(uint32));

	/* reserve space for the JEntries of the keys and values */
	metaoffset = reserveFromBuffer(buffer, sizeof(JEntry) * val->val.object.nPairs * 2);

	totallen = 0;
	for (i = 0; i < val->val.object.nPairs; i++)
	{
		JsonbPair *pair = &val->val.object.pairs[i];
		int len;
		JEntry meta;

		/* put key */
		convertJsonbScalar(buffer, &meta, &pair->key);

		len = meta & JENTRY_POSMASK;
		totallen += len;

		if (totallen > JENTRY_POSMASK)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("total size of jsonb array elements exceeds the maximum of %u bytes",
							JENTRY_POSMASK)));

		if (i == 0)
			meta |= JENTRY_ISFIRST;
		else
			meta = (meta & ~JENTRY_POSMASK) | totallen;
		copyToBuffer(buffer, metaoffset, (char *) &meta, sizeof(JEntry));
		metaoffset += sizeof(JEntry);

		convertJsonbValue(buffer, &meta, &pair->value, level);
		len = meta & JENTRY_POSMASK;
		totallen += len;

		if (totallen > JENTRY_POSMASK)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("total size of jsonb array elements exceeds the maximum of %u bytes",
							JENTRY_POSMASK)));

		meta = (meta & ~JENTRY_POSMASK) | totallen;
		copyToBuffer(buffer, metaoffset, (char *) &meta, sizeof(JEntry));
		metaoffset += sizeof(JEntry);
	}

	totallen = buffer->len - offset;

	*pheader = JENTRY_ISCONTAINER | totallen;
}

static void
convertJsonbScalar(convertState *buffer, JEntry *jentry, JsonbValue *scalarVal)
{
	int			numlen;
	short		padlen;

	switch (scalarVal->type)
	{
		case jbvNull:
			*jentry = JENTRY_ISNULL;
			break;

		case jbvString:
			appendToBuffer(buffer, scalarVal->val.string.val, scalarVal->val.string.len);

			*jentry = scalarVal->val.string.len;
			break;

		case jbvNumeric:
			numlen = VARSIZE_ANY(scalarVal->val.numeric);
			padlen = padBufferToInt(buffer);

			appendToBuffer(buffer, (char *) scalarVal->val.numeric, numlen);

			*jentry = JENTRY_ISNUMERIC | (padlen + numlen);
			break;

		case jbvBool:
			*jentry = (scalarVal->val.boolean) ?
				JENTRY_ISBOOL_TRUE : JENTRY_ISBOOL_FALSE;
			break;

		default:
			elog(ERROR, "invalid jsonb scalar type");
	}
}

/*
 * Initialize an iterator for iterating all elements in a container.
 */
static void
iteratorFromContainer(JsonbIterator *it, JsonbContainer *container)
{
	it->containerType = container->header & (JB_FARRAY | JB_FOBJECT);
	it->nElems = container->header & JB_CMASK;
	it->buffer = (char *) container;

	/* Array starts just after header */
	it->meta = container->children;
	it->state = jbi_start;

	switch (it->containerType)
	{
		case JB_FARRAY:
			it->dataProper =
				(char *) it->meta + it->nElems * sizeof(JEntry);
			it->isScalar = (container->header & JB_FSCALAR) != 0;
			/* This is either a "raw scalar", or an array */
			Assert(!it->isScalar || it->nElems == 1);
			break;
		case JB_FOBJECT:

			/*
			 * Offset reflects that nElems indicates JsonbPairs in an object.
			 * Each key and each value contain Jentry metadata just the same.
			 */
			it->dataProper =
				(char *) it->meta + it->nElems * sizeof(JEntry) * 2;
			break;
		default:
			elog(ERROR, "unknown type of jsonb container");
	}
}

/*
 * JsonbIteratorNext() worker
 *
 * Returns bool indicating if v was a non-jbvBinary container, and thus if
 * further recursion is required by caller (according to its skipNested
 * preference).  If it is required, we set the caller's iterator for further
 * recursion into the nested value.  If we're going to skip nested items, just
 * set v to a jbvBinary value, but don't set caller's iterator.
 *
 * Unlike with containers (either in this function or in any
 * JsonbIteratorNext() infrastructure), we fully convert from what is
 * ultimately a Jsonb on-disk representation, to a JsonbValue in-memory
 * representation (for scalar values only).  JsonbIteratorNext() initializes
 * container Jsonbvalues, but without a sane private buffer.  For scalar values
 * it has to be done for real (even if we don't actually allocate more memory
 * to do this.  The point is that our JsonbValues scalars can be passed around
 * anywhere).
 */
static bool
formIterIsContainer(JsonbIterator **it, JsonbValue *val, JEntry *ent,
					bool skipNested)
{
	fillJsonbValue(ent, (*it)->dataProper, val);

	if (IsAJsonbScalar(val) || skipNested)
		return false;
	else
	{
		/*
		 * It's a container type, so setup caller's iterator to point to
		 * that, and return indication of that.
		 *
		 * Get child iterator.
		 */
		JsonbIterator *child = palloc(sizeof(JsonbIterator));

		iteratorFromContainer(child, val->val.binary.data);

		child->parent = *it;
		*it = child;

		return true;
	}
}

/*
 * JsonbIteratorNext() worker:	Return parent, while freeing memory for current
 * iterator
 */
static JsonbIterator *
freeAndGetParent(JsonbIterator *it)
{
	JsonbIterator *v = it->parent;

	pfree(it);
	return v;
}

/*
 * pushJsonbValue() worker:  Iteration-like forming of Jsonb
 */
static JsonbParseState *
pushState(JsonbParseState **pstate)
{
	JsonbParseState *ns = palloc(sizeof(JsonbParseState));

	ns->next = *pstate;
	return ns;
}

/*
 * pushJsonbValue() worker:  Append a pair key to state when generating a Jsonb
 */
static void
appendKey(JsonbParseState *pstate, JsonbValue *string)
{
	JsonbValue *object = &pstate->contVal;

	Assert(object->type == jbvObject);
	Assert(string->type == jbvString);

	if (object->val.object.nPairs >= JSONB_MAX_PAIRS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of jsonb object pairs exceeds the maximum allowed (%zu)",
						JSONB_MAX_PAIRS)));

	if (object->val.object.nPairs >= pstate->size)
	{
		pstate->size *= 2;
		object->val.object.pairs = repalloc(object->val.object.pairs,
											sizeof(JsonbPair) * pstate->size);
	}

	object->val.object.pairs[object->val.object.nPairs].key = *string;
	object->val.object.pairs[object->val.object.nPairs].order = object->val.object.nPairs;
}

/*
 * pushJsonbValue() worker:  Append a pair value to state when generating a
 * Jsonb
 */
static void
appendValue(JsonbParseState *pstate, JsonbValue *scalarVal)
{
	JsonbValue *object = &pstate->contVal;

	Assert(object->type == jbvObject);

	object->val.object.pairs[object->val.object.nPairs++].value = *scalarVal;
}

/*
 * pushJsonbValue() worker:  Append an element to state when generating a Jsonb
 */
static void
appendElement(JsonbParseState *pstate, JsonbValue *scalarVal)
{
	JsonbValue *array = &pstate->contVal;

	Assert(array->type == jbvArray);

	if (array->val.array.nElems >= JSONB_MAX_ELEMS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of jsonb array elements exceeds the maximum allowed (%zu)",
						JSONB_MAX_ELEMS)));

	if (array->val.array.nElems >= pstate->size)
	{
		pstate->size *= 2;
		array->val.array.elems = repalloc(array->val.array.elems,
										  sizeof(JsonbValue) * pstate->size);
	}

	array->val.array.elems[array->val.array.nElems++] = *scalarVal;
}

/*
 * Compare two jbvString JsonbValue values, a and b.
 *
 * This is a special qsort_arg() comparator used to sort strings in certain
 * internal contexts where it is sufficient to have a well-defined sort order.
 * In particular, object pair keys are sorted according to this criteria to
 * facilitate cheap binary searches where we don't care about lexical sort
 * order.
 *
 * a and b are first sorted based on their length.  If a tie-breaker is
 * required, only then do we consider string binary equality.
 *
 * Third argument 'binequal' may point to a bool. If it's set, *binequal is set
 * to true iff a and b have full binary equality, since some callers have an
 * interest in whether the two values are equal or merely equivalent.
 */
static int
lengthCompareJsonbStringValue(const void *a, const void *b, void *binequal)
{
	const JsonbValue *va = (const JsonbValue *) a;
	const JsonbValue *vb = (const JsonbValue *) b;
	int			res;

	Assert(va->type == jbvString);
	Assert(vb->type == jbvString);

	if (va->val.string.len == vb->val.string.len)
	{
		res = memcmp(va->val.string.val, vb->val.string.val, va->val.string.len);
		if (res == 0 && binequal)
			*((bool *) binequal) = true;
	}
	else
	{
		res = (va->val.string.len > vb->val.string.len) ? 1 : -1;
	}

	return res;
}

/*
 * qsort_arg() comparator to compare JsonbPair values.
 *
 * Function implemented in terms of lengthCompareJsonbStringValue(), and thus the
 * same "arg setting" hack will be applied here in respect of the pair's key
 * values.
 *
 * N.B: String comparisons here are "length-wise"
 *
 * Pairs with equals keys are ordered such that the order field is respected.
 */
static int
lengthCompareJsonbPair(const void *a, const void *b, void *binequal)
{
	const JsonbPair *pa = (const JsonbPair *) a;
	const JsonbPair *pb = (const JsonbPair *) b;
	int			res;

	res = lengthCompareJsonbStringValue(&pa->key, &pb->key, binequal);

	/*
	 * Guarantee keeping order of equal pair.  Unique algorithm will prefer
	 * first element as value.
	 */
	if (res == 0)
		res = (pa->order > pb->order) ? -1 : 1;

	return res;
}

/*
 * Sort and unique-ify pairs in JsonbValue object
 */
static void
uniqueifyJsonbObject(JsonbValue *object)
{
	bool		hasNonUniq = false;

	Assert(object->type == jbvObject);

	if (object->val.object.nPairs > 1)
		qsort_arg(object->val.object.pairs, object->val.object.nPairs, sizeof(JsonbPair),
				  lengthCompareJsonbPair, &hasNonUniq);

	if (hasNonUniq)
	{
		JsonbPair  *ptr = object->val.object.pairs + 1,
				   *res = object->val.object.pairs;

		while (ptr - object->val.object.pairs < object->val.object.nPairs)
		{
			/* Avoid copying over duplicate */
			if (lengthCompareJsonbStringValue(ptr, res, NULL) != 0)
			{
				res++;
				if (ptr != res)
					memcpy(res, ptr, sizeof(JsonbPair));
			}
			ptr++;
		}

		object->val.object.nPairs = res + 1 - object->val.object.pairs;
	}
}
