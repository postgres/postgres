/*-------------------------------------------------------------------------
 *
 * params.h
 *	  Support for finding the values associated with Param nodes.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/nodes/params.h,v 1.33 2006/10/04 00:30:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARAMS_H
#define PARAMS_H


/* ----------------
 *	  ParamListInfo
 *
 *	  ParamListInfo arrays are used to pass parameters into the executor
 *	  for parameterized plans.	Each entry in the array defines the value
 *	  to be substituted for a PARAM_EXTERN parameter.  The "paramid"
 *	  of a PARAM_EXTERN Param can range from 1 to numParams.
 *
 *	  Although parameter numbers are normally consecutive, we allow
 *	  ptype == InvalidOid to signal an unused array entry.
 *
 *	  PARAM_FLAG_CONST signals the planner that it may treat this parameter
 *	  as a constant (i.e., generate a plan that works only for this value
 *	  of the parameter).
 *
 *	  Although the data structure is really an array, not a list, we keep
 *	  the old typedef name to avoid unnecessary code changes.
 * ----------------
 */

#define PARAM_FLAG_CONST	0x0001		/* parameter is constant */

typedef struct ParamExternData
{
	Datum		value;			/* parameter value */
	bool		isnull;			/* is it NULL? */
	uint16		pflags;			/* flag bits, see above */
	Oid			ptype;			/* parameter's datatype, or 0 */
} ParamExternData;

typedef struct ParamListInfoData
{
	int			numParams;		/* number of ParamExternDatas following */
	ParamExternData params[1];	/* VARIABLE LENGTH ARRAY */
} ParamListInfoData;

typedef ParamListInfoData *ParamListInfo;


/* ----------------
 *	  ParamExecData
 *
 *	  ParamExecData entries are used for executor internal parameters
 *	  (that is, values being passed into or out of a sub-query).  The
 *	  paramid of a PARAM_EXEC Param is a (zero-based) index into an
 *	  array of ParamExecData records, which is referenced through
 *	  es_param_exec_vals or ecxt_param_exec_vals.
 *
 *	  If execPlan is not NULL, it points to a SubPlanState node that needs
 *	  to be executed to produce the value.	(This is done so that we can have
 *	  lazy evaluation of InitPlans: they aren't executed until/unless a
 *	  result value is needed.)	Otherwise the value is assumed to be valid
 *	  when needed.
 * ----------------
 */

typedef struct ParamExecData
{
	void	   *execPlan;		/* should be "SubPlanState *" */
	Datum		value;
	bool		isnull;
} ParamExecData;


/* Functions found in src/backend/nodes/params.c */
extern ParamListInfo copyParamList(ParamListInfo from);

#endif   /* PARAMS_H */
