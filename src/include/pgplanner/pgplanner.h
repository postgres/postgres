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
	Oid			oprcode;	/* implementing function OID */
	Oid			oprleft;
	Oid			oprright;
	Oid			oprresult;
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

/* ----------------------------------------------------------------
 *	Callback function pointer types
 * ----------------------------------------------------------------
 */

/* Look up a relation by schema + name. Return NULL if not found. */
typedef PgPlannerRelationInfo *(*pgplanner_relation_hook_type)(
	const char *schemaname, const char *relname);

/* Look up a relation by OID. Return NULL if not found. */
typedef PgPlannerRelationInfo *(*pgplanner_relation_by_oid_hook_type)(
	Oid relid);

/* Look up an operator by name and argument types. Return NULL if not found. */
typedef PgPlannerOperatorInfo *(*pgplanner_operator_hook_type)(
	const char *opname, Oid left_type, Oid right_type);

/* Look up type info by OID. Return NULL if not found. */
typedef PgPlannerTypeInfo *(*pgplanner_type_hook_type)(Oid typid);

/* Look up function info by OID. Return NULL if not found. */
typedef PgPlannerFunctionInfo *(*pgplanner_function_hook_type)(Oid funcid);

/*
 * Look up function candidates by name. Returns the number of candidates
 * found. *candidates_out is set to point to an array of PgPlannerFuncCandidate.
 * Return 0 if no functions match.
 */
typedef int (*pgplanner_func_candidates_hook_type)(
	const char *funcname, PgPlannerFuncCandidate **candidates_out);

/* Look up aggregate info by function OID. Return NULL if not found. */
typedef PgPlannerAggregateInfo *(*pgplanner_aggregate_hook_type)(Oid aggfnoid);

/* ----------------------------------------------------------------
 *	Callback registration struct
 * ----------------------------------------------------------------
 */

typedef struct PgPlannerCallbacks
{
	pgplanner_relation_hook_type		get_relation;
	pgplanner_relation_by_oid_hook_type	get_relation_by_oid;
	pgplanner_operator_hook_type		get_operator;
	pgplanner_type_hook_type			get_type;
	pgplanner_function_hook_type		get_function;
	pgplanner_func_candidates_hook_type	get_func_candidates;
	pgplanner_aggregate_hook_type		get_aggregate;
} PgPlannerCallbacks;

/* ----------------------------------------------------------------
 *	Library API
 * ----------------------------------------------------------------
 */

/* Initialize the planner library (call once at startup). */
extern void pgplanner_init(void);

/*
 * Plan a SQL query. Callbacks are set for the duration of planning and
 * protected by a mutex, so this is safe to call from multiple threads
 * (calls will serialize).
 */
extern PlannedStmt *pgplanner_plan_query(const char *sql,
										 const PgPlannerCallbacks *callbacks);

/* ----------------------------------------------------------------
 *	Internal helpers (used by modified PG code, not by callers)
 * ----------------------------------------------------------------
 */

/* Get the currently active callbacks (valid only during planning). */
extern const PgPlannerCallbacks *pgplanner_get_callbacks(void);

/* Build a Relation from callback-provided info. */
extern Relation pgplanner_build_relation(const PgPlannerRelationInfo *info);

#endif							/* PGPLANNER_H */
