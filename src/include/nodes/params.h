/*-------------------------------------------------------------------------
 *
 * params.h
 *	  Declarations of stuff needed to handle parameterized plans.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: params.h,v 1.23 2003/08/04 02:40:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARAMS_H
#define PARAMS_H

#include "access/attnum.h"


/* ----------------
 * The following are the possible values for the 'paramkind'
 * field of a Param node.
 *
 * PARAM_NAMED: The parameter has a name, i.e. something
 *				like `$.salary' or `$.foobar'.
 *				In this case field `paramname' must be a valid name.
 *
 * PARAM_NUM:	The parameter has only a numeric identifier,
 *				i.e. something like `$1', `$2' etc.
 *				The number is contained in the `paramid' field.
 *
 * PARAM_EXEC:	The parameter is an internal executor parameter.
 *				It has a number contained in the `paramid' field.
 *
 * PARAM_INVALID should never appear in a Param node; it's used to mark
 * the end of a ParamListInfo array.
 *
 * NOTE: As of PostgreSQL 7.3, named parameters aren't actually used and
 * so the code that handles PARAM_NAMED cases is dead code.  We leave it
 * in place since it might be resurrected someday.
 * ----------------
 */

#define PARAM_NAMED		11
#define PARAM_NUM		12
#define PARAM_EXEC		15
#define PARAM_INVALID	100


/* ----------------
 *	  ParamListInfo
 *
 *	  ParamListInfo entries are used to pass parameters into the executor
 *	  for parameterized plans.	Each entry in the array defines the value
 *	  to be substituted for a PARAM_NAMED or PARAM_NUM parameter.
 *
 *		kind   : the kind of parameter (PARAM_NAMED or PARAM_NUM)
 *		name   : the parameter name (valid if kind == PARAM_NAMED)
 *		id	   : the parameter id (valid if kind == PARAM_NUM)
 *		isnull : true if the value is null (if so 'value' is undefined)
 *		value  : the value that has to be substituted in the place
 *				 of the parameter.
 *
 *	 ParamListInfo is to be used as an array of ParamListInfoData
 *	 records.  A dummy record with kind == PARAM_INVALID marks the end
 *	 of the array.
 * ----------------
 */

typedef struct ParamListInfoData
{
	int			kind;
	char	   *name;
	AttrNumber	id;
	bool		isnull;
	Datum		value;
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

#endif   /* PARAMS_H */
