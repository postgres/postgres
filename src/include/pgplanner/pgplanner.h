/*-------------------------------------------------------------------------
 *
 * pgplanner.h
 *	  Public API for the standalone PostgreSQL planner library.
 *
 *	  External engines register callbacks to provide relation, operator,
 *	  type, and function metadata. The library uses these callbacks instead
 *	  of querying PostgreSQL system catalogs.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPLANNER_H
#define PGPLANNER_H

#include "postgres.h"
#include "nodes/plannodes.h"
#include "utils/relcache.h"

/* ----------------------------------------------------------------
 *	Callback data structures
 * ----------------------------------------------------------------
 */

/* Column definition provided by the caller */
typedef struct PgPlannerColumn
{
	const char *colname;
	Oid			typid;
	int32		typmod;		/* -1 for default */
} PgPlannerColumn;

/* Relation info returned by the relation callback */
typedef struct PgPlannerRelationInfo
{
	Oid			relid;
	const char *relname;
	char		relkind;	/* RELKIND_RELATION, RELKIND_VIEW, etc. */
	int			natts;
	PgPlannerColumn *columns;
} PgPlannerRelationInfo;

/* Operator info returned by the operator callback */
typedef struct PgPlannerOperatorInfo
{
	Oid			oprid;
	const char *oprname;		/* operator name (e.g. "=") */
	Oid			oprnamespace;	/* 0 => PG_CATALOG_NAMESPACE */
	Oid			oprowner;		/* 0 => BOOTSTRAP_SUPERUSERID */
	char		oprkind;		/* 'b' binary, 'l' prefix; 0 => 'b' */
	bool		oprcanmerge;
	bool		oprcanhash;
	Oid			oprcode;		/* implementing function OID */
	Oid			oprleft;
	Oid			oprright;
	Oid			oprresult;
	Oid			oprcom;			/* commutator OID, 0 if none */
	Oid			oprnegate;		/* negator OID, 0 if none */
	Oid			oprrest;		/* restriction estimator, 0 if none */
	Oid			oprjoin;		/* join estimator, 0 if none */
} PgPlannerOperatorInfo;

/* Type info returned by the type callback */
typedef struct PgPlannerTypeInfo
{
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typtype;		/* 'b' base, 'c' composite, 'd' domain, 'e' enum, 'p' pseudo, 'r' range */
	Oid			typbasetype;	/* for domains, 0 otherwise */
	int32		typtypmod;		/* for domains, -1 otherwise */
	const char *typname;		/* type name (e.g. "int4") */
	Oid			typnamespace;	/* OID of namespace, e.g. PG_CATALOG_NAMESPACE */
	Oid			typowner;
	char		typcategory;	/* 'N' numeric, 'S' string, etc. */
	bool		typispreferred;
	bool		typisdefined;	/* true for real types */
	char		typdelim;		/* delimiter for arrays, usually ',' */
	Oid			typrelid;		/* 0 if not composite */
	Oid			typsubscript;	/* subscript handler func OID, 0 if none */
	Oid			typelem;		/* element type if array, 0 otherwise */
	Oid			typarray;		/* array type OID, 0 if none */
	Oid			typinput;		/* input function OID */
	Oid			typoutput;		/* output function OID */
	Oid			typreceive;		/* binary input function, 0 if none */
	Oid			typsend;		/* binary output function, 0 if none */
	Oid			typmodin;		/* typmod input function, 0 if none */
	Oid			typmodout;		/* typmod output function, 0 if none */
	Oid			typanalyze;		/* custom analyze function, 0 if none */
	char		typstorage;		/* 'p' plain, 'x' extended, 'e' external, 'm' main */
	bool		typnotnull;		/* NOT NULL constraint (domains) */
	int32		typndims;		/* array dimensions for domain, 0 otherwise */
	Oid			typcollation;	/* collation OID, 0 if not collatable */
} PgPlannerTypeInfo;

/* Function info returned by the function callback (pg_proc fields) */
typedef struct PgPlannerFunctionInfo
{
	bool		retset;
	Oid			rettype;
	char		prokind;		/* 'f' function, 'a' aggregate, 'w' window, 'p' procedure */
	bool		proisstrict;
	int			pronargs;
	Oid		   *proargtypes;	/* array of pronargs Oids, NULL if pronargs=0 */
	Oid			provariadic;	/* InvalidOid if not variadic */
	/* Additional fields needed by the planner (sensible defaults used if 0) */
	const char *proname;		/* function name, NULL => "unknown" */
	Oid			pronamespace;	/* namespace OID, 0 => PG_CATALOG_NAMESPACE */
	char		provolatile;	/* 'i' immutable, 's' stable, 'v' volatile; 0 => 'i' */
	char		proparallel;	/* 's' safe, 'r' restricted, 'u' unsafe; 0 => 's' */
	bool		proleakproof;
	float4		procost;		/* estimated execution cost; 0 => 1 */
	float4		prorows;		/* estimated # of rows out (if proretset) */
	int16		pronargdefaults;/* number of arguments with defaults */
	Oid			prosupport;		/* planner support function, 0 if none */
} PgPlannerFunctionInfo;

/* Function candidate for name-based lookup */
typedef struct PgPlannerFuncCandidate
{
	Oid			oid;
	int			nargs;
	Oid		   *argtypes;		/* array of nargs Oids, NULL if nargs=0 */
	Oid			variadic_type;	/* InvalidOid if not variadic */
	int			ndargs;			/* number of defaulted args */
} PgPlannerFuncCandidate;

/* Aggregate info (pg_aggregate fields) */
typedef struct PgPlannerAggregateInfo
{
	char		aggkind;		/* 'n' normal, 'o' ordered-set, 'h' hypothetical */
	int			aggnumdirectargs;
	Oid			aggtransfn;
	Oid			aggfinalfn;
	Oid			aggcombinefn;
	Oid			aggserialfn;
	Oid			aggdeserialfn;
	Oid			aggtranstype;
	int32		aggtransspace;
	char		aggfinalmodify;	/* 'r' read-only, 's' shareable, 'w' read-write */
	Oid			aggsortop;
	const char *agginitval;		/* NULL if none */
} PgPlannerAggregateInfo;

/* Cast info (pg_cast fields) */
typedef struct PgPlannerCastInfo
{
	Oid			castfunc;		/* cast function OID, 0 if binary-coercible */
	char		castcontext;	/* 'i' implicit, 'a' assignment, 'e' explicit */
	char		castmethod;		/* 'f' function, 'b' binary, 'i' inout */
} PgPlannerCastInfo;

/* ----------------------------------------------------------------
 *	Callback function pointer types
 * ----------------------------------------------------------------
 */

typedef PgPlannerRelationInfo *(*pgplanner_relation_hook_type)(
	const char *schemaname, const char *relname);

typedef PgPlannerRelationInfo *(*pgplanner_relation_by_oid_hook_type)(
	Oid relid);

typedef PgPlannerOperatorInfo *(*pgplanner_operator_hook_type)(
	const char *opname, Oid left_type, Oid right_type);

typedef PgPlannerOperatorInfo *(*pgplanner_operator_by_oid_hook_type)(
	Oid oproid);

typedef PgPlannerTypeInfo *(*pgplanner_type_hook_type)(Oid typid);

typedef PgPlannerFunctionInfo *(*pgplanner_function_hook_type)(Oid funcid);

typedef int (*pgplanner_func_candidates_hook_type)(
	const char *funcname, PgPlannerFuncCandidate **candidates_out);

typedef PgPlannerAggregateInfo *(*pgplanner_aggregate_hook_type)(Oid aggfnoid);

typedef PgPlannerCastInfo *(*pgplanner_cast_hook_type)(Oid source, Oid target);

/* ----------------------------------------------------------------
 *	Callback registration struct
 * ----------------------------------------------------------------
 */

typedef struct PgPlannerCallbacks
{
	pgplanner_relation_hook_type		get_relation;
	pgplanner_relation_by_oid_hook_type	get_relation_by_oid;
	pgplanner_operator_hook_type		get_operator;
	pgplanner_operator_by_oid_hook_type	get_operator_by_oid;
	pgplanner_type_hook_type			get_type;
	pgplanner_function_hook_type		get_function;
	pgplanner_func_candidates_hook_type	get_func_candidates;
	pgplanner_aggregate_hook_type		get_aggregate;
	pgplanner_cast_hook_type			get_cast;
} PgPlannerCallbacks;

/* ----------------------------------------------------------------
 *	Library API
 * ----------------------------------------------------------------
 */

extern void pgplanner_init(void);

extern PlannedStmt *pgplanner_plan_query(const char *sql,
										 const PgPlannerCallbacks *callbacks);

/* ----------------------------------------------------------------
 *	Internal helpers (used by modified PG code, not by callers)
 * ----------------------------------------------------------------
 */

extern const PgPlannerCallbacks *pgplanner_get_callbacks(void);

extern Relation pgplanner_build_relation(const PgPlannerRelationInfo *info);

#endif							/* PGPLANNER_H */
