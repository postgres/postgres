/*-------------------------------------------------------------------------
 *
 * params.h--
 *    Declarations/definitions of stuff needed to handle parameterized plans.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: params.h,v 1.1 1996/08/28 01:57:39 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARAMS_H
#define PARAMS_H

#include "postgres.h"
#include "access/attnum.h"

/* ----------------------------------------------------------------
 *
 * The following are the possible values for the 'paramkind'
 * field of a Param node.
 *    
 * PARAM_NAMED: The parameter has a name, i.e. something
 *              like `$.salary' or `$.foobar'.
 *              In this case field `paramname' must be a valid Name.
 *              and field `paramid' must be == 0.
 *
 * PARAM_NUM:   The parameter has only a numeric identifier,
 *              i.e. something like `$1', `$2' etc.
 *              The number is contained in the `parmid' field.
 *
 * PARAM_NEW:   Used in PRS2 rule, similar to PARAM_NAMED.
 *              The `paramname' & `paramid' refer to the "NEW" tuple
 *		`paramname' is the attribute name and `paramid' its
 *		attribute number.
 *              
 * PARAM_OLD:   Same as PARAM_NEW, but in this case we refer to
 *		the "OLD" tuple.
 */

#define PARAM_NAMED	11
#define PARAM_NUM	12
#define PARAM_NEW	13
#define PARAM_OLD	14
#define PARAM_INVALID   100


/* ----------------------------------------------------------------
 *    ParamListInfo
 *
 *    Information needed in order for the executor to handle
 *    parameterized plans (you know,  $.salary, $.name etc. stuff...).
 *
 *    ParamListInfoData contains information needed when substituting a
 *    Param node with a Const node.
 *
 *	kind   : the kind of parameter.
 *      name   : the parameter name (valid if kind == PARAM_NAMED,
 *               PARAM_NEW or PARAM_OLD)
 *      id     : the parameter id (valid if kind == PARAM_NUM)
 *		 or the attrno (if kind == PARAM_NEW or PARAM_OLD)
 *      type   : PG_TYPE OID of the value
 *      length : length in bytes of the value
 *      isnull : true if & only if the value is null (if true then
 *               the fields 'length' and 'value' are undefined).
 *      value  : the value that has to be substituted in the place
 *               of the parameter.
 *
 *   ParamListInfo is to be used as an array of ParamListInfoData
 *   records. An 'InvalidName' in the name field of such a record
 *   indicates that this is the last record in the array.
 *
 * ----------------------------------------------------------------
 */

typedef struct ParamListInfoData {
    int			kind;
    char 		*name;
    AttrNumber		id;
    Oid			type;
    Size		length;
    bool		isnull;
    bool		byval;
    Datum		value;
} ParamListInfoData;

typedef ParamListInfoData *ParamListInfo;

#endif	/* PARAMS_H */
