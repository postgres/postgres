/*-------------------------------------------------------------------------
 *
 * catalog_utils.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/Attic/catalog_utils.c,v 1.28 1997/09/18 20:21:05 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"

#include "lib/dllist.h"
#include "utils/datum.h"

#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "fmgr.h"

#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "utils/syscache.h"
#include "catalog/catname.h"

#include "parser/catalog_utils.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "catalog/pg_proc.h"
#include "catalog/indexing.h"
#include "catalog/catname.h"

#include "access/skey.h"
#include "access/relscan.h"
#include "access/tupdesc.h"
#include "access/htup.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "access/itup.h"
#include "access/tupmacs.h"

#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "utils/lsyscache.h"
#include "storage/lmgr.h"

#include "port-protos.h"		/* strdup() */

struct
{
	char	   *field;
	int			code;
}			special_attr[] =

{
	{
		"ctid", SelfItemPointerAttributeNumber
	},
	{
		"oid", ObjectIdAttributeNumber
	},
	{
		"xmin", MinTransactionIdAttributeNumber
	},
	{
		"cmin", MinCommandIdAttributeNumber
	},
	{
		"xmax", MaxTransactionIdAttributeNumber
	},
	{
		"cmax", MaxCommandIdAttributeNumber
	},
	{
		"chain", ChainItemPointerAttributeNumber
	},
	{
		"anchor", AnchorItemPointerAttributeNumber
	},
	{
		"tmin", MinAbsoluteTimeAttributeNumber
	},
	{
		"tmax", MaxAbsoluteTimeAttributeNumber
	},
	{
		"vtype", VersionTypeAttributeNumber
	}
};

#define SPECIALS (sizeof(special_attr)/sizeof(*special_attr))

static char *attnum_type[SPECIALS] = {
	"tid",
	"oid",
	"xid",
	"cid",
	"xid",
	"cid",
	"tid",
	"tid",
	"abstime",
	"abstime",
	"char"
};

#define MAXFARGS 8				/* max # args to a c or postquel function */

/*
 *	This structure is used to explore the inheritance hierarchy above
 *	nodes in the type tree in order to disambiguate among polymorphic
 *	functions.
 */

typedef struct _InhPaths
{
	int			nsupers;		/* number of superclasses */
	Oid			self;			/* this class */
	Oid		   *supervec;		/* vector of superclasses */
} InhPaths;

/*
 *	This structure holds a list of possible functions or operators that
 *	agree with the known name and argument types of the function/operator.
 */
typedef struct _CandidateList
{
	Oid		   *args;
	struct _CandidateList *next;
}		   *CandidateList;

static Oid **argtype_inherit(int nargs, Oid *oid_array);
static Oid **genxprod(InhPaths *arginh, int nargs);
static int	findsupers(Oid relid, Oid **supervec);
static bool check_typeid(Oid id);
static char *instr1(TypeTupleForm tp, char *string, int typlen);
static void op_error(char *op, Oid arg1, Oid arg2);

/* check to see if a type id is valid,
 * returns true if it is. By using this call before calling
 * get_id_type or get_id_typname, more meaningful error messages
 * can be produced because the caller typically has more context of
 *	what's going on                 - jolly
 */
static bool
check_typeid(Oid id)
{
	return (SearchSysCacheTuple(TYPOID,
								ObjectIdGetDatum(id),
								0, 0, 0) != NULL);
}


/* return a Type structure, given an typid */
Type
get_id_type(Oid id)
{
	HeapTuple	tup;

	if (!(tup = SearchSysCacheTuple(TYPOID, ObjectIdGetDatum(id),
									0, 0, 0)))
	{
		elog(WARN, "type id lookup of %ud failed", id);
		return (NULL);
	}
	return ((Type) tup);
}

/* return a type name, given a typeid */
char	   *
get_id_typname(Oid id)
{
	HeapTuple	tup;
	TypeTupleForm typetuple;

	if (!(tup = SearchSysCacheTuple(TYPOID, ObjectIdGetDatum(id),
									0, 0, 0)))
	{
		elog(WARN, "type id lookup of %ud failed", id);
		return (NULL);
	}
	typetuple = (TypeTupleForm) GETSTRUCT(tup);
	return (typetuple->typname).data;
}

/* return a Type structure, given type name */
Type
type(char *s)
{
	HeapTuple	tup;

	if (s == NULL)
	{
		elog(WARN, "type(): Null type");
	}

	if (!(tup = SearchSysCacheTuple(TYPNAME, PointerGetDatum(s), 0, 0, 0)))
	{
		elog(WARN, "type name lookup of %s failed", s);
	}
	return ((Type) tup);
}

/* given attribute id, return type of that attribute */
/* XXX Special case for pseudo-attributes is a hack */
Oid
att_typeid(Relation rd, int attid)
{

	if (attid < 0)
	{
		return (typeid(type(attnum_type[-attid - 1])));
	}

	/*
	 * -1 because varattno (where attid comes from) returns one more than
	 * index
	 */
	return (rd->rd_att->attrs[attid - 1]->atttypid);
}


int
att_attnelems(Relation rd, int attid)
{
	return (rd->rd_att->attrs[attid - 1]->attnelems);
}

/* given type, return the type OID */
Oid
typeid(Type tp)
{
	if (tp == NULL)
	{
		elog(WARN, "typeid() called with NULL type struct");
	}
	return (tp->t_oid);
}

/* given type (as type struct), return the length of type */
int16
tlen(Type t)
{
	TypeTupleForm typ;

	typ = (TypeTupleForm) GETSTRUCT(t);
	return (typ->typlen);
}

/* given type (as type struct), return the value of its 'byval' attribute.*/
bool
tbyval(Type t)
{
	TypeTupleForm typ;

	typ = (TypeTupleForm) GETSTRUCT(t);
	return (typ->typbyval);
}

/* given type (as type struct), return the name of type */
char	   *
tname(Type t)
{
	TypeTupleForm typ;

	typ = (TypeTupleForm) GETSTRUCT(t);
	return (typ->typname).data;
}

/* given type (as type struct), return wether type is passed by value */
int
tbyvalue(Type t)
{
	TypeTupleForm typ;

	typ = (TypeTupleForm) GETSTRUCT(t);
	return (typ->typbyval);
}

/* given a type, return its typetype ('c' for 'c'atalog types) */
static char
typetypetype(Type t)
{
	TypeTupleForm typ;

	typ = (TypeTupleForm) GETSTRUCT(t);
	return (typ->typtype);
}

/* given operator, return the operator OID */
Oid
oprid(Operator op)
{
	return (op->t_oid);
}

/*
 *	given opname, leftTypeId and rightTypeId,
 *	find all possible (arg1, arg2) pairs for which an operator named
 *	opname exists, such that leftTypeId can be coerced to arg1 and
 *	rightTypeId can be coerced to arg2
 */
static int
binary_oper_get_candidates(char *opname,
						   Oid leftTypeId,
						   Oid rightTypeId,
						   CandidateList *candidates)
{
	CandidateList current_candidate;
	Relation	pg_operator_desc;
	HeapScanDesc pg_operator_scan;
	HeapTuple	tup;
	OperatorTupleForm oper;
	Buffer		buffer;
	int			nkeys;
	int			ncandidates = 0;
	ScanKeyData opKey[3];

	*candidates = NULL;

	ScanKeyEntryInitialize(&opKey[0], 0,
						   Anum_pg_operator_oprname,
						   NameEqualRegProcedure,
						   NameGetDatum(opname));

	ScanKeyEntryInitialize(&opKey[1], 0,
						   Anum_pg_operator_oprkind,
						   CharacterEqualRegProcedure,
						   CharGetDatum('b'));


	if (leftTypeId == UNKNOWNOID)
	{
		if (rightTypeId == UNKNOWNOID)
		{
			nkeys = 2;
		}
		else
		{
			nkeys = 3;

			ScanKeyEntryInitialize(&opKey[2], 0,
								   Anum_pg_operator_oprright,
								   ObjectIdEqualRegProcedure,
								   ObjectIdGetDatum(rightTypeId));
		}
	}
	else if (rightTypeId == UNKNOWNOID)
	{
		nkeys = 3;

		ScanKeyEntryInitialize(&opKey[2], 0,
							   Anum_pg_operator_oprleft,
							   ObjectIdEqualRegProcedure,
							   ObjectIdGetDatum(leftTypeId));
	}
	else
	{
		/* currently only "unknown" can be coerced */
		return 0;
	}

	pg_operator_desc = heap_openr(OperatorRelationName);
	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  SelfTimeQual,
									  nkeys,
									  opKey);

	do
	{
		tup = heap_getnext(pg_operator_scan, 0, &buffer);
		if (HeapTupleIsValid(tup))
		{
			current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
			current_candidate->args = (Oid *) palloc(2 * sizeof(Oid));

			oper = (OperatorTupleForm) GETSTRUCT(tup);
			current_candidate->args[0] = oper->oprleft;
			current_candidate->args[1] = oper->oprright;
			current_candidate->next = *candidates;
			*candidates = current_candidate;
			ncandidates++;
			ReleaseBuffer(buffer);
		}
	} while (HeapTupleIsValid(tup));

	heap_endscan(pg_operator_scan);
	heap_close(pg_operator_desc);

	return ncandidates;
}

/*
 * equivalentOpersAfterPromotion -
 *	  checks if a list of candidate operators obtained from
 *	  binary_oper_get_candidates() contain equivalent operators. If
 *	  this routine is called, we have more than 1 candidate and need to
 *	  decided whether to pick one of them. This routine returns true if
 *	  the all the candidates operate on the same data types after
 *	  promotion (int2, int4, float4 -> float8).
 */
static bool
equivalentOpersAfterPromotion(CandidateList candidates)
{
	CandidateList result;
	CandidateList promotedCandidates = NULL;
	Oid			leftarg,
				rightarg;

	for (result = candidates; result != NULL; result = result->next)
	{
		CandidateList c;

		c = (CandidateList) palloc(sizeof(*c));
		c->args = (Oid *) palloc(2 * sizeof(Oid));
		switch (result->args[0])
		{
			case FLOAT4OID:
			case INT4OID:
			case INT2OID:
			case CASHOID:
				c->args[0] = FLOAT8OID;
				break;
			default:
				c->args[0] = result->args[0];
				break;
		}
		switch (result->args[1])
		{
			case FLOAT4OID:
			case INT4OID:
			case INT2OID:
			case CASHOID:
				c->args[1] = FLOAT8OID;
				break;
			default:
				c->args[1] = result->args[1];
				break;
		}
		c->next = promotedCandidates;
		promotedCandidates = c;
	}

	/*
	 * if we get called, we have more than 1 candidates so we can do the
	 * following safely
	 */
	leftarg = promotedCandidates->args[0];
	rightarg = promotedCandidates->args[1];

	for (result = promotedCandidates->next; result != NULL; result = result->next)
	{
		if (result->args[0] != leftarg || result->args[1] != rightarg)

			/*
			 * this list contains operators that operate on different data
			 * types even after promotion. Hence we can't decide on which
			 * one to pick. The user must do explicit type casting.
			 */
			return FALSE;
	}

	/*
	 * all the candidates are equivalent in the following sense: they
	 * operate on equivalent data types and picking any one of them is as
	 * good.
	 */
	return TRUE;
}


/*
 *	given a choice of argument type pairs for a binary operator,
 *	try to choose a default pair
 */
static CandidateList
binary_oper_select_candidate(Oid arg1,
							 Oid arg2,
							 CandidateList candidates)
{
	CandidateList result;

	/*
	 * if both are "unknown", there is no way to select a candidate
	 *
	 * current wisdom holds that the default operator should be one in which
	 * both operands have the same type (there will only be one such
	 * operator)
	 *
	 * 7.27.93 - I have decided not to do this; it's too hard to justify, and
	 * it's easy enough to typecast explicitly -avi [the rest of this
	 * routine were commented out since then -ay]
	 */

	if (arg1 == UNKNOWNOID && arg2 == UNKNOWNOID)
		return (NULL);

	/*
	 * 6/23/95 - I don't complete agree with avi. In particular, casting
	 * floats is a pain for users. Whatever the rationale behind not doing
	 * this is, I need the following special case to work.
	 *
	 * In the WHERE clause of a query, if a float is specified without
	 * quotes, we treat it as float8. I added the float48* operators so
	 * that we can operate on float4 and float8. But now we have more than
	 * one matching operator if the right arg is unknown (eg. float
	 * specified with quotes). This break some stuff in the regression
	 * test where there are floats in quotes not properly casted. Below is
	 * the solution. In addition to requiring the operator operates on the
	 * same type for both operands [as in the code Avi originally
	 * commented out], we also require that the operators be equivalent in
	 * some sense. (see equivalentOpersAfterPromotion for details.) - ay
	 * 6/95
	 */
	if (!equivalentOpersAfterPromotion(candidates))
		return NULL;

	/*
	 * if we get here, any one will do but we're more picky and require
	 * both operands be the same.
	 */
	for (result = candidates; result != NULL; result = result->next)
	{
		if (result->args[0] == result->args[1])
			return result;
	}

	return (NULL);
}

/* Given operator, types of arg1, and arg2, return oper struct */
/* arg1, arg2 --typeids */
Operator
oper(char *op, Oid arg1, Oid arg2, bool noWarnings)
{
	HeapTuple	tup;
	CandidateList candidates;
	int			ncandidates;

	if (!arg2)
		arg2 = arg1;
	if (!arg1)
		arg1 = arg2;

	if (!(tup = SearchSysCacheTuple(OPRNAME,
									PointerGetDatum(op),
									ObjectIdGetDatum(arg1),
									ObjectIdGetDatum(arg2),
									Int8GetDatum('b'))))
	{
		ncandidates = binary_oper_get_candidates(op, arg1, arg2, &candidates);
		if (ncandidates == 0)
		{

			/*
			 * no operators of the desired types found
			 */
			if (!noWarnings)
				op_error(op, arg1, arg2);
			return (NULL);
		}
		else if (ncandidates == 1)
		{

			/*
			 * exactly one operator of the desired types found
			 */
			tup = SearchSysCacheTuple(OPRNAME,
									  PointerGetDatum(op),
								   ObjectIdGetDatum(candidates->args[0]),
								   ObjectIdGetDatum(candidates->args[1]),
									  Int8GetDatum('b'));
			Assert(HeapTupleIsValid(tup));
		}
		else
		{

			/*
			 * multiple operators of the desired types found
			 */
			candidates = binary_oper_select_candidate(arg1, arg2, candidates);
			if (candidates != NULL)
			{
				/* we chose one of them */
				tup = SearchSysCacheTuple(OPRNAME,
										  PointerGetDatum(op),
								   ObjectIdGetDatum(candidates->args[0]),
								   ObjectIdGetDatum(candidates->args[1]),
										  Int8GetDatum('b'));
				Assert(HeapTupleIsValid(tup));
			}
			else
			{
				Type		tp1,
							tp2;

				/* we chose none of them */
				tp1 = get_id_type(arg1);
				tp2 = get_id_type(arg2);
				if (!noWarnings)
				{
					elog(NOTICE, "there is more than one operator %s for types", op);
					elog(NOTICE, "%s and %s. You will have to retype this query",
						 tname(tp1), tname(tp2));
					elog(WARN, "using an explicit cast");
				}
				return (NULL);
			}
		}
	}
	return ((Operator) tup);
}

/*
 *	given opname and typeId, find all possible types for which
 *	a right/left unary operator named opname exists,
 *	such that typeId can be coerced to it
 */
static int
unary_oper_get_candidates(char *op,
						  Oid typeId,
						  CandidateList *candidates,
						  char rightleft)
{
	CandidateList current_candidate;
	Relation	pg_operator_desc;
	HeapScanDesc pg_operator_scan;
	HeapTuple	tup;
	OperatorTupleForm oper;
	Buffer		buffer;
	int			ncandidates = 0;

	static ScanKeyData opKey[2] = {
		{0, Anum_pg_operator_oprname, NameEqualRegProcedure},
	{0, Anum_pg_operator_oprkind, CharacterEqualRegProcedure}};

	*candidates = NULL;

	fmgr_info(NameEqualRegProcedure, (func_ptr *) &opKey[0].sk_func,
			  &opKey[0].sk_nargs);
	opKey[0].sk_argument = NameGetDatum(op);
	fmgr_info(CharacterEqualRegProcedure, (func_ptr *) &opKey[1].sk_func,
			  &opKey[1].sk_nargs);
	opKey[1].sk_argument = CharGetDatum(rightleft);

	/* currently, only "unknown" can be coerced */

	/*
	 * but we should allow types that are internally the same to be
	 * "coerced"
	 */
	if (typeId != UNKNOWNOID)
	{
		return 0;
	}

	pg_operator_desc = heap_openr(OperatorRelationName);
	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  SelfTimeQual,
									  2,
									  opKey);

	do
	{
		tup = heap_getnext(pg_operator_scan, 0, &buffer);
		if (HeapTupleIsValid(tup))
		{
			current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
			current_candidate->args = (Oid *) palloc(sizeof(Oid));

			oper = (OperatorTupleForm) GETSTRUCT(tup);
			if (rightleft == 'r')
				current_candidate->args[0] = oper->oprleft;
			else
				current_candidate->args[0] = oper->oprright;
			current_candidate->next = *candidates;
			*candidates = current_candidate;
			ncandidates++;
			ReleaseBuffer(buffer);
		}
	} while (HeapTupleIsValid(tup));

	heap_endscan(pg_operator_scan);
	heap_close(pg_operator_desc);

	return ncandidates;
}

/* Given unary right-side operator (operator on right), return oper struct */
/* arg-- type id */
Operator
right_oper(char *op, Oid arg)
{
	HeapTuple	tup;
	CandidateList candidates;
	int			ncandidates;

	/*
	 * if (!OpCache) { init_op_cache(); }
	 */
	if (!(tup = SearchSysCacheTuple(OPRNAME,
									PointerGetDatum(op),
									ObjectIdGetDatum(arg),
									ObjectIdGetDatum(InvalidOid),
									Int8GetDatum('r'))))
	{
		ncandidates = unary_oper_get_candidates(op, arg, &candidates, 'r');
		if (ncandidates == 0)
		{
			elog(WARN,
				 "Can't find right op: %s for type %d", op, arg);
			return (NULL);
		}
		else if (ncandidates == 1)
		{
			tup = SearchSysCacheTuple(OPRNAME,
									  PointerGetDatum(op),
								   ObjectIdGetDatum(candidates->args[0]),
									  ObjectIdGetDatum(InvalidOid),
									  Int8GetDatum('r'));
			Assert(HeapTupleIsValid(tup));
		}
		else
		{
			elog(NOTICE, "there is more than one right operator %s", op);
			elog(NOTICE, "you will have to retype this query");
			elog(WARN, "using an explicit cast");
			return (NULL);
		}
	}
	return ((Operator) tup);
}

/* Given unary left-side operator (operator on left), return oper struct */
/* arg--type id */
Operator
left_oper(char *op, Oid arg)
{
	HeapTuple	tup;
	CandidateList candidates;
	int			ncandidates;

	/*
	 * if (!OpCache) { init_op_cache(); }
	 */
	if (!(tup = SearchSysCacheTuple(OPRNAME,
									PointerGetDatum(op),
									ObjectIdGetDatum(InvalidOid),
									ObjectIdGetDatum(arg),
									Int8GetDatum('l'))))
	{
		ncandidates = unary_oper_get_candidates(op, arg, &candidates, 'l');
		if (ncandidates == 0)
		{
			elog(WARN,
				 "Can't find left op: %s for type %d", op, arg);
			return (NULL);
		}
		else if (ncandidates == 1)
		{
			tup = SearchSysCacheTuple(OPRNAME,
									  PointerGetDatum(op),
									  ObjectIdGetDatum(InvalidOid),
								   ObjectIdGetDatum(candidates->args[0]),
									  Int8GetDatum('l'));
			Assert(HeapTupleIsValid(tup));
		}
		else
		{
			elog(NOTICE, "there is more than one left operator %s", op);
			elog(NOTICE, "you will have to retype this query");
			elog(WARN, "using an explicit cast");
			return (NULL);
		}
	}
	return ((Operator) tup);
}

/* given range variable, return id of variable */

int
varattno(Relation rd, char *a)
{
	int			i;

	for (i = 0; i < rd->rd_rel->relnatts; i++)
	{
		if (!namestrcmp(&(rd->rd_att->attrs[i]->attname), a))
		{
			return (i + 1);
		}
	}
	for (i = 0; i < SPECIALS; i++)
	{
		if (!strcmp(special_attr[i].field, a))
		{
			return (special_attr[i].code);
		}
	}

	elog(WARN, "Relation %s does not have attribute %s",
		 RelationGetRelationName(rd), a);
	return (-1);
}

/* Given range variable, return whether attribute of this name
 * is a set.
 * NOTE the ASSUMPTION here that no system attributes are, or ever
 * will be, sets.
 */
bool
varisset(Relation rd, char *name)
{
	int			i;

	/* First check if this is a system attribute */
	for (i = 0; i < SPECIALS; i++)
	{
		if (!strcmp(special_attr[i].field, name))
		{
			return (false);		/* no sys attr is a set */
		}
	}
	return (get_attisset(rd->rd_id, name));
}

/* given range variable, return id of variable */
int
nf_varattno(Relation rd, char *a)
{
	int			i;

	for (i = 0; i < rd->rd_rel->relnatts; i++)
	{
		if (!namestrcmp(&(rd->rd_att->attrs[i]->attname), a))
		{
			return (i + 1);
		}
	}
	for (i = 0; i < SPECIALS; i++)
	{
		if (!strcmp(special_attr[i].field, a))
		{
			return (special_attr[i].code);
		}
	}
	return InvalidAttrNumber;
}

/*-------------
 * given an attribute number and a relation, return its relation name
 */
char	   *
getAttrName(Relation rd, int attrno)
{
	char	   *name;
	int			i;

	if (attrno < 0)
	{
		for (i = 0; i < SPECIALS; i++)
		{
			if (special_attr[i].code == attrno)
			{
				name = special_attr[i].field;
				return (name);
			}
		}
		elog(WARN, "Illegal attr no %d for relation %s",
			 attrno, RelationGetRelationName(rd));
	}
	else if (attrno >= 1 && attrno <= RelationGetNumberOfAttributes(rd))
	{
		name = (rd->rd_att->attrs[attrno - 1]->attname).data;
		return (name);
	}
	else
	{
		elog(WARN, "Illegal attr no %d for relation %s",
			 attrno, RelationGetRelationName(rd));
	}

	/*
	 * Shouldn't get here, but we want lint to be happy...
	 */

	return (NULL);
}

/* Given a typename and value, returns the ascii form of the value */

#ifdef NOT_USED
char	   *
outstr(char *typename,			/* Name of type of value */
	   char *value)				/* Could be of any type */
{
	TypeTupleForm tp;
	Oid			op;

	tp = (TypeTupleForm) GETSTRUCT(type(typename));
	op = tp->typoutput;
	return ((char *) fmgr(op, value));
}

#endif

/* Given a Type and a string, return the internal form of that string */
char	   *
instr2(Type tp, char *string, int typlen)
{
	return (instr1((TypeTupleForm) GETSTRUCT(tp), string, typlen));
}

/* Given a type structure and a string, returns the internal form of
   that string */
static char *
instr1(TypeTupleForm tp, char *string, int typlen)
{
	Oid			op;
	Oid			typelem;

	op = tp->typinput;
	typelem = tp->typelem;		/* XXX - used for array_in */
	/* typlen is for bpcharin() and varcharin() */
	return ((char *) fmgr(op, string, typelem, typlen));
}

/* Given the attribute type of an array return the arrtribute type of
   an element of the array */

Oid
GetArrayElementType(Oid typearray)
{
	HeapTuple	type_tuple;
	TypeTupleForm type_struct_array;

	type_tuple = SearchSysCacheTuple(TYPOID,
									 ObjectIdGetDatum(typearray),
									 0, 0, 0);

	if (!HeapTupleIsValid(type_tuple))
		elog(WARN, "GetArrayElementType: Cache lookup failed for type %d",
			 typearray);

	/* get the array type struct from the type tuple */
	type_struct_array = (TypeTupleForm) GETSTRUCT(type_tuple);

	if (type_struct_array->typelem == InvalidOid)
	{
		elog(WARN, "GetArrayElementType: type %s is not an array",
			 (Name) &(type_struct_array->typname.data[0]));
	}

	return (type_struct_array->typelem);
}

Oid
funcid_get_rettype(Oid funcid)
{
	HeapTuple	func_tuple = NULL;
	Oid			funcrettype = (Oid) 0;

	func_tuple = SearchSysCacheTuple(PROOID, ObjectIdGetDatum(funcid),
									 0, 0, 0);

	if (!HeapTupleIsValid(func_tuple))
		elog(WARN, "function  %d does not exist", funcid);

	funcrettype = (Oid)
		((Form_pg_proc) GETSTRUCT(func_tuple))->prorettype;

	return (funcrettype);
}

/*
 * get a list of all argument type vectors for which a function named
 * funcname taking nargs arguments exists
 */
static CandidateList
func_get_candidates(char *funcname, int nargs)
{
	Relation	heapRelation;
	Relation	idesc;
	ScanKeyData skey;
	HeapTuple	tuple;
	IndexScanDesc sd;
	RetrieveIndexResult indexRes;
	Buffer		buffer;
	Form_pg_proc pgProcP;
	bool		bufferUsed = FALSE;
	CandidateList candidates = NULL;
	CandidateList current_candidate;
	int			i;

	heapRelation = heap_openr(ProcedureRelationName);
	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) NameEqualRegProcedure,
						   (Datum) funcname);

	idesc = index_openr(ProcedureNameIndex);

	sd = index_beginscan(idesc, false, 1, &skey);

	do
	{
		tuple = (HeapTuple) NULL;
		if (bufferUsed)
		{
			ReleaseBuffer(buffer);
			bufferUsed = FALSE;
		}

		indexRes = index_getnext(sd, ForwardScanDirection);
		if (indexRes)
		{
			ItemPointer iptr;

			iptr = &indexRes->heap_iptr;
			tuple = heap_fetch(heapRelation, NowTimeQual, iptr, &buffer);
			pfree(indexRes);
			if (HeapTupleIsValid(tuple))
			{
				pgProcP = (Form_pg_proc) GETSTRUCT(tuple);
				bufferUsed = TRUE;
				if (pgProcP->pronargs == nargs)
				{
					current_candidate = (CandidateList)
						palloc(sizeof(struct _CandidateList));
					current_candidate->args = (Oid *)
						palloc(8 * sizeof(Oid));
					MemSet(current_candidate->args, 0, 8 * sizeof(Oid));
					for (i = 0; i < nargs; i++)
					{
						current_candidate->args[i] =
							pgProcP->proargtypes[i];
					}

					current_candidate->next = candidates;
					candidates = current_candidate;
				}
			}
		}
	} while (indexRes);

	index_endscan(sd);
	index_close(idesc);
	heap_close(heapRelation);

	return candidates;
}

/*
 * can input_typeids be coerced to func_typeids?
 */
static bool
can_coerce(int nargs, Oid *input_typeids, Oid *func_typeids)
{
	int			i;
	Type		tp;

	/*
	 * right now, we only coerce "unknown", and we cannot coerce it to a
	 * relation type
	 */
	for (i = 0; i < nargs; i++)
	{
		if (input_typeids[i] != func_typeids[i])
		{
			if ((input_typeids[i] == BPCHAROID && func_typeids[i] == TEXTOID) ||
				(input_typeids[i] == BPCHAROID && func_typeids[i] == VARCHAROID) ||
				(input_typeids[i] == VARCHAROID && func_typeids[i] == TEXTOID) ||
				(input_typeids[i] == VARCHAROID && func_typeids[i] == BPCHAROID) ||
			(input_typeids[i] == CASHOID && func_typeids[i] == INT4OID) ||
			 (input_typeids[i] == INT4OID && func_typeids[i] == CASHOID))
				;				/* these are OK */
			else if (input_typeids[i] != UNKNOWNOID || func_typeids[i] == 0)
				return false;

			tp = get_id_type(input_typeids[i]);
			if (typetypetype(tp) == 'c')
				return false;
		}
	}

	return true;
}

/*
 * given a list of possible typeid arrays to a function and an array of
 * input typeids, produce a shortlist of those function typeid arrays
 * that match the input typeids (either exactly or by coercion), and
 * return the number of such arrays
 */
static int
match_argtypes(int nargs,
			   Oid *input_typeids,
			   CandidateList function_typeids,
			   CandidateList *candidates)		/* return value */
{
	CandidateList current_candidate;
	CandidateList matching_candidate;
	Oid		   *current_typeids;
	int			ncandidates = 0;

	*candidates = NULL;

	for (current_candidate = function_typeids;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeids = current_candidate->args;
		if (can_coerce(nargs, input_typeids, current_typeids))
		{
			matching_candidate = (CandidateList)
				palloc(sizeof(struct _CandidateList));
			matching_candidate->args = current_typeids;
			matching_candidate->next = *candidates;
			*candidates = matching_candidate;
			ncandidates++;
		}
	}

	return ncandidates;
}

/*
 * given the input argtype array and more than one candidate
 * for the function argtype array, attempt to resolve the conflict.
 * returns the selected argtype array if the conflict can be resolved,
 * otherwise returns NULL
 */
static Oid *
func_select_candidate(int nargs,
					  Oid *input_typeids,
					  CandidateList candidates)
{
	/* XXX no conflict resolution implemeneted yet */
	return (NULL);
}

bool
func_get_detail(char *funcname,
				int nargs,
				Oid *oid_array,
				Oid *funcid,	/* return value */
				Oid *rettype,	/* return value */
				bool *retset,	/* return value */
				Oid **true_typeids)		/* return value */
{
	Oid		  **input_typeid_vector;
	Oid		   *current_input_typeids;
	CandidateList function_typeids;
	CandidateList current_function_typeids;
	HeapTuple	ftup;
	Form_pg_proc pform;

	/*
	 * attempt to find named function in the system catalogs with
	 * arguments exactly as specified - so that the normal case is just as
	 * quick as before
	 */
	ftup = SearchSysCacheTuple(PRONAME,
							   PointerGetDatum(funcname),
							   Int32GetDatum(nargs),
							   PointerGetDatum(oid_array),
							   0);
	*true_typeids = oid_array;

	/*
	 * If an exact match isn't found : 1) get a vector of all possible
	 * input arg type arrays constructed from the superclasses of the
	 * original input arg types 2) get a list of all possible argument
	 * type arrays to the function with given name and number of arguments
	 * 3) for each input arg type array from vector #1 : a) find how many
	 * of the function arg type arrays from list #2 it can be coerced to
	 * b) - if the answer is one, we have our function - if the answer is
	 * more than one, attempt to resolve the conflict - if the answer is
	 * zero, try the next array from vector #1
	 */
	if (!HeapTupleIsValid(ftup))
	{
		function_typeids = func_get_candidates(funcname, nargs);

		if (function_typeids != NULL)
		{
			int			ncandidates = 0;

			input_typeid_vector = argtype_inherit(nargs, oid_array);
			current_input_typeids = oid_array;

			do
			{
				ncandidates = match_argtypes(nargs, current_input_typeids,
											 function_typeids,
											 &current_function_typeids);
				if (ncandidates == 1)
				{
					*true_typeids = current_function_typeids->args;
					ftup = SearchSysCacheTuple(PRONAME,
											   PointerGetDatum(funcname),
											   Int32GetDatum(nargs),
										  PointerGetDatum(*true_typeids),
											   0);
					Assert(HeapTupleIsValid(ftup));
				}
				else if (ncandidates > 1)
				{
					*true_typeids =
						func_select_candidate(nargs,
											  current_input_typeids,
											  current_function_typeids);
					if (*true_typeids == NULL)
					{
						elog(NOTICE, "there is more than one function named \"%s\"",
							 funcname);
						elog(NOTICE, "that satisfies the given argument types. you will have to");
						elog(NOTICE, "retype your query using explicit typecasts.");
						func_error("func_get_detail", funcname, nargs, oid_array);
					}
					else
					{
						ftup = SearchSysCacheTuple(PRONAME,
											   PointerGetDatum(funcname),
												   Int32GetDatum(nargs),
										  PointerGetDatum(*true_typeids),
												   0);
						Assert(HeapTupleIsValid(ftup));
					}
				}
				current_input_typeids = *input_typeid_vector++;
			}
			while (current_input_typeids !=
				   InvalidOid && ncandidates == 0);
		}
	}

	if (!HeapTupleIsValid(ftup))
	{
		Type		tp;

		if (nargs == 1)
		{
			tp = get_id_type(oid_array[0]);
			if (typetypetype(tp) == 'c')
				elog(WARN, "no such attribute or function \"%s\"",
					 funcname);
		}
		func_error("func_get_detail", funcname, nargs, oid_array);
	}
	else
	{
		pform = (Form_pg_proc) GETSTRUCT(ftup);
		*funcid = ftup->t_oid;
		*rettype = pform->prorettype;
		*retset = pform->proretset;

		return (true);
	}
/* shouldn't reach here */
	return (false);

}

/*
 *	argtype_inherit() -- Construct an argtype vector reflecting the
 *						 inheritance properties of the supplied argv.
 *
 *		This function is used to disambiguate among functions with the
 *		same name but different signatures.  It takes an array of eight
 *		type ids.  For each type id in the array that's a complex type
 *		(a class), it walks up the inheritance tree, finding all
 *		superclasses of that type.	A vector of new Oid type arrays
 *		is returned to the caller, reflecting the structure of the
 *		inheritance tree above the supplied arguments.
 *
 *		The order of this vector is as follows:  all superclasses of the
 *		rightmost complex class are explored first.  The exploration
 *		continues from right to left.  This policy means that we favor
 *		keeping the leftmost argument type as low in the inheritance tree
 *		as possible.  This is intentional; it is exactly what we need to
 *		do for method dispatch.  The last type array we return is all
 *		zeroes.  This will match any functions for which return types are
 *		not defined.  There are lots of these (mostly builtins) in the
 *		catalogs.
 */
static Oid **
argtype_inherit(int nargs, Oid *oid_array)
{
	Oid			relid;
	int			i;
	InhPaths	arginh[MAXFARGS];

	for (i = 0; i < MAXFARGS; i++)
	{
		if (i < nargs)
		{
			arginh[i].self = oid_array[i];
			if ((relid = typeid_get_relid(oid_array[i])) != InvalidOid)
			{
				arginh[i].nsupers = findsupers(relid, &(arginh[i].supervec));
			}
			else
			{
				arginh[i].nsupers = 0;
				arginh[i].supervec = (Oid *) NULL;
			}
		}
		else
		{
			arginh[i].self = InvalidOid;
			arginh[i].nsupers = 0;
			arginh[i].supervec = (Oid *) NULL;
		}
	}

	/* return an ordered cross-product of the classes involved */
	return (genxprod(arginh, nargs));
}

typedef struct _SuperQE
{
	Oid			sqe_relid;
} SuperQE;

static int
findsupers(Oid relid, Oid **supervec)
{
	Oid		   *relidvec;
	Relation	inhrel;
	HeapScanDesc inhscan;
	ScanKeyData skey;
	HeapTuple	inhtup;
	TupleDesc	inhtupdesc;
	int			nvisited;
	SuperQE    *qentry,
			   *vnode;
	Dllist	   *visited,
			   *queue;
	Dlelem	   *qe,
			   *elt;

	Relation	rd;
	Buffer		buf;
	Datum		d;
	bool		newrelid;
	char		isNull;

	nvisited = 0;
	queue = DLNewList();
	visited = DLNewList();


	inhrel = heap_openr(InheritsRelationName);
	RelationSetLockForRead(inhrel);
	inhtupdesc = RelationGetTupleDescriptor(inhrel);

	/*
	 * Use queue to do a breadth-first traversal of the inheritance graph
	 * from the relid supplied up to the root.
	 */
	do
	{
		ScanKeyEntryInitialize(&skey, 0x0, Anum_pg_inherits_inhrel,
							   ObjectIdEqualRegProcedure,
							   ObjectIdGetDatum(relid));

		inhscan = heap_beginscan(inhrel, 0, NowTimeQual, 1, &skey);

		while (HeapTupleIsValid(inhtup = heap_getnext(inhscan, 0, &buf)))
		{
			qentry = (SuperQE *) palloc(sizeof(SuperQE));

			d = fastgetattr(inhtup, Anum_pg_inherits_inhparent,
							inhtupdesc, &isNull);
			qentry->sqe_relid = DatumGetObjectId(d);

			/* put this one on the queue */
			DLAddTail(queue, DLNewElem(qentry));

			ReleaseBuffer(buf);
		}

		heap_endscan(inhscan);

		/* pull next unvisited relid off the queue */
		do
		{
			qe = DLRemHead(queue);
			qentry = qe ? (SuperQE *) DLE_VAL(qe) : NULL;

			if (qentry == (SuperQE *) NULL)
				break;

			relid = qentry->sqe_relid;
			newrelid = true;

			for (elt = DLGetHead(visited); elt; elt = DLGetSucc(elt))
			{
				vnode = (SuperQE *) DLE_VAL(elt);
				if (vnode && (qentry->sqe_relid == vnode->sqe_relid))
				{
					newrelid = false;
					break;
				}
			}
		} while (!newrelid);

		if (qentry != (SuperQE *) NULL)
		{

			/* save the type id, rather than the relation id */
			if ((rd = heap_open(qentry->sqe_relid)) == (Relation) NULL)
				elog(WARN, "relid %d does not exist", qentry->sqe_relid);
			qentry->sqe_relid = typeid(type(RelationGetRelationName(rd)->data));
			heap_close(rd);

			DLAddTail(visited, qe);

			nvisited++;
		}
	} while (qentry != (SuperQE *) NULL);

	RelationUnsetLockForRead(inhrel);
	heap_close(inhrel);

	if (nvisited > 0)
	{
		relidvec = (Oid *) palloc(nvisited * sizeof(Oid));
		*supervec = relidvec;

		for (elt = DLGetHead(visited); elt; elt = DLGetSucc(elt))
		{
			vnode = (SuperQE *) DLE_VAL(elt);
			*relidvec++ = vnode->sqe_relid;
		}

	}
	else
	{
		*supervec = (Oid *) NULL;
	}

	return (nvisited);
}

static Oid **
genxprod(InhPaths *arginh, int nargs)
{
	int			nanswers;
	Oid		  **result,
			  **iter;
	Oid		   *oneres;
	int			i,
				j;
	int			cur[MAXFARGS];

	nanswers = 1;
	for (i = 0; i < nargs; i++)
	{
		nanswers *= (arginh[i].nsupers + 2);
		cur[i] = 0;
	}

	iter = result = (Oid **) palloc(sizeof(Oid *) * nanswers);

	/* compute the cross product from right to left */
	for (;;)
	{
		oneres = (Oid *) palloc(MAXFARGS * sizeof(Oid));
		MemSet(oneres, 0, MAXFARGS * sizeof(Oid));

		for (i = nargs - 1; i >= 0 && cur[i] > arginh[i].nsupers; i--)
			continue;

		/* if we're done, terminate with NULL pointer */
		if (i < 0)
		{
			*iter = NULL;
			return (result);
		}

		/* no, increment this column and zero the ones after it */
		cur[i] = cur[i] + 1;
		for (j = nargs - 1; j > i; j--)
			cur[j] = 0;

		for (i = 0; i < nargs; i++)
		{
			if (cur[i] == 0)
				oneres[i] = arginh[i].self;
			else if (cur[i] > arginh[i].nsupers)
				oneres[i] = 0;	/* wild card */
			else
				oneres[i] = arginh[i].supervec[cur[i] - 1];
		}

		*iter++ = oneres;
	}
}

/* Given a type id, returns the in-conversion function of the type */
Oid
typeid_get_retinfunc(Oid type_id)
{
	HeapTuple	typeTuple;
	TypeTupleForm type;
	Oid			infunc;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type_id),
									0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(WARN, "typeid_get_retinfunc: Invalid type - oid = %u", type_id);

	type = (TypeTupleForm) GETSTRUCT(typeTuple);
	infunc = type->typinput;
	return (infunc);
}

/* Given a type id, returns the out-conversion function of the type */
Oid
typeid_get_retoutfunc(Oid type_id)
{
	HeapTuple	typeTuple;
	TypeTupleForm type;
	Oid			outfunc;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type_id),
									0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(WARN, "typeid_get_retoutfunc: Invalid type - oid = %u", type_id);

	type = (TypeTupleForm) GETSTRUCT(typeTuple);
	outfunc = type->typoutput;
	return (outfunc);
}

Oid
typeid_get_relid(Oid type_id)
{
	HeapTuple	typeTuple;
	TypeTupleForm type;
	Oid			infunc;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type_id),
									0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(WARN, "typeid_get_relid: Invalid type - oid = %u", type_id);

	type = (TypeTupleForm) GETSTRUCT(typeTuple);
	infunc = type->typrelid;
	return (infunc);
}

Oid
get_typrelid(Type typ)
{
	TypeTupleForm typtup;

	typtup = (TypeTupleForm) GETSTRUCT(typ);

	return (typtup->typrelid);
}

Oid
get_typelem(Oid type_id)
{
	HeapTuple	typeTuple;
	TypeTupleForm type;

	if (!(typeTuple = SearchSysCacheTuple(TYPOID,
										  ObjectIdGetDatum(type_id),
										  0, 0, 0)))
	{
		elog(WARN, "type id lookup of %u failed", type_id);
	}
	type = (TypeTupleForm) GETSTRUCT(typeTuple);

	return (type->typelem);
}

#ifdef NOT_USED
char
FindDelimiter(char *typename)
{
	char		delim;
	HeapTuple	typeTuple;
	TypeTupleForm type;


	if (!(typeTuple = SearchSysCacheTuple(TYPNAME,
										  PointerGetDatum(typename),
										  0, 0, 0)))
	{
		elog(WARN, "type name lookup of %s failed", typename);
	}
	type = (TypeTupleForm) GETSTRUCT(typeTuple);

	delim = type->typdelim;
	return (delim);
}

#endif

/*
 * Give a somewhat useful error message when the operator for two types
 * is not found.
 */
static void
op_error(char *op, Oid arg1, Oid arg2)
{
	Type		tp1 = NULL,
				tp2 = NULL;

	if (check_typeid(arg1))
	{
		tp1 = get_id_type(arg1);
	}
	else
	{
		elog(WARN, "left hand side of operator %s has an unknown type, probably a bad attribute name", op);
	}

	if (check_typeid(arg2))
	{
		tp2 = get_id_type(arg2);
	}
	else
	{
		elog(WARN, "right hand side of operator %s has an unknown type, probably a bad attribute name", op);
	}

	elog(NOTICE, "there is no operator %s for types %s and %s",
		 op, tname(tp1), tname(tp2));
	elog(NOTICE, "You will either have to retype this query using an");
	elog(NOTICE, "explicit cast, or you will have to define the operator");
	elog(WARN, "%s for %s and %s using CREATE OPERATOR",
		 op, tname(tp1), tname(tp2));
}

/*
 * Error message when function lookup fails that gives details of the
 * argument types
 */
void
func_error(char *caller, char *funcname, int nargs, Oid *argtypes)
{
	char		p[(NAMEDATALEN + 2) * MAXFMGRARGS],
			   *ptr;
	int			i;

	ptr = p;
	*ptr = '\0';
	for (i = 0; i < nargs; i++)
	{
		if (i)
		{
			*ptr++ = ',';
			*ptr++ = ' ';
		}
		if (argtypes[i] != 0)
		{
			strcpy(ptr, tname(get_id_type(argtypes[i])));
			*(ptr + NAMEDATALEN) = '\0';
		}
		else
			strcpy(ptr, "opaque");
		ptr += strlen(ptr);
	}

	elog(WARN, "%s: function %s(%s) does not exist", caller, funcname, p);
}

/*
 * Error message when aggregate lookup fails that gives details of the
 * basetype
 */
void
agg_error(char *caller, char *aggname, Oid basetypeID)
{

	/*
	 * basetypeID that is Invalid (zero) means aggregate over all types.
	 * (count)
	 */

	if (basetypeID == InvalidOid)
	{
		elog(WARN, "%s: aggregate '%s' for all types does not exist", caller, aggname);
	}
	else
	{
		elog(WARN, "%s: aggregate '%s' for '%s' does not exist", caller, aggname,
			 tname(get_id_type(basetypeID)));
	}
}
