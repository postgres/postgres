/**********************************************************************
 * pl_handler.c		- Handler for the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/pl/plpgsql/src/pl_handler.c,v 1.2 1998/09/01 04:40:25 momjian Exp $
 *
 *	  This software is copyrighted by Jan Wieck - Hamburg.
 *
 *	  The author hereby grants permission  to  use,  copy,	modify,
 *	  distribute,  and	license this software and its documentation
 *	  for any purpose, provided that existing copyright notices are
 *	  retained	in	all  copies  and  that	this notice is included
 *	  verbatim in any distributions. No written agreement, license,
 *	  or  royalty  fee	is required for any of the authorized uses.
 *	  Modifications to this software may be  copyrighted  by  their
 *	  author  and  need  not  follow  the licensing terms described
 *	  here, provided that the new terms are  clearly  indicated  on
 *	  the first page of each file where they apply.
 *
 *	  IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY
 *	  PARTY  FOR  DIRECT,	INDIRECT,	SPECIAL,   INCIDENTAL,	 OR
 *	  CONSEQUENTIAL   DAMAGES  ARISING	OUT  OF  THE  USE  OF  THIS
 *	  SOFTWARE, ITS DOCUMENTATION, OR ANY DERIVATIVES THEREOF, EVEN
 *	  IF  THE  AUTHOR  HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH
 *	  DAMAGE.
 *
 *	  THE  AUTHOR  AND	DISTRIBUTORS  SPECIFICALLY	 DISCLAIM	ANY
 *	  WARRANTIES,  INCLUDING,  BUT	NOT  LIMITED  TO,  THE	IMPLIED
 *	  WARRANTIES  OF  MERCHANTABILITY,	FITNESS  FOR  A  PARTICULAR
 *	  PURPOSE,	AND NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON
 *	  AN "AS IS" BASIS, AND THE AUTHOR	AND  DISTRIBUTORS  HAVE  NO
 *	  OBLIGATION   TO	PROVIDE   MAINTENANCE,	 SUPPORT,  UPDATES,
 *	  ENHANCEMENTS, OR MODIFICATIONS.
 *
 **********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "plpgsql.h"
#include "pl.tab.h"

#include "executor/spi.h"
#include "commands/trigger.h"
#include "utils/elog.h"
#include "utils/builtins.h"
#include "fmgr.h"
#include "access/heapam.h"

#include "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"


static PLpgSQL_function *compiled_functions = NULL;


Datum plpgsql_call_handler(FmgrInfo *proinfo,
					 FmgrValues *proargs, bool *isNull);

static Datum plpgsql_func_handler(FmgrInfo *proinfo,
					 FmgrValues *proargs, bool *isNull);

static HeapTuple plpgsql_trigger_handler(FmgrInfo *proinfo);


/* ----------
 * plpgsql_call_handler		- This is the only visible function
 *				  of the PL interpreter. The PostgreSQL
 *				  function manager and trigger manager
 *				  call this function for execution of
 *				  PL/pgSQL procedures.
 * ----------
 */
Datum
plpgsql_call_handler(FmgrInfo *proinfo,
					 FmgrValues *proargs,
					 bool *isNull)
{
	Datum		retval;

	/* ----------
	 * Connect to SPI manager
	 * ----------
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "plpgsql: cannot connect to SPI manager");

	/* ----------
	 * Determine if called as function or trigger and
	 * call appropriate subhandler
	 * ----------
	 */
	if (CurrentTriggerData == NULL)
		retval = plpgsql_func_handler(proinfo, proargs, isNull);
	else
		retval = (Datum) plpgsql_trigger_handler(proinfo);

	/* ----------
	 * Disconnect from SPI manager
	 * ----------
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "plpgsql: SPI_finish() failed");

	return retval;
}


/* ----------
 * plpgsql_func_handler()	- Handler for regular function calls
 * ----------
 */
static Datum
plpgsql_func_handler(FmgrInfo *proinfo,
					 FmgrValues *proargs,
					 bool *isNull)
{
	PLpgSQL_function *func;

	/* ----------
	 * Check if we already compiled this function
	 * ----------
	 */
	for (func = compiled_functions; func != NULL; func = func->next)
	{
		if (proinfo->fn_oid == func->fn_oid)
			break;
	}

	/* ----------
	 * If not, do so and add it to the compiled ones
	 * ----------
	 */
	if (func == NULL)
	{
		func = plpgsql_compile(proinfo->fn_oid, T_FUNCTION);

		func->next = compiled_functions;
		compiled_functions = func;
	}

	return plpgsql_exec_function(func, proargs, isNull);
}


/* ----------
 * plpgsql_trigger_handler()	- Handler for trigger calls
 * ----------
 */
static HeapTuple
plpgsql_trigger_handler(FmgrInfo *proinfo)
{
	TriggerData *trigdata;
	PLpgSQL_function *func;

	/* ----------
	 * Save the current trigger data local
	 * ----------
	 */
	trigdata = CurrentTriggerData;
	CurrentTriggerData = NULL;

	/* ----------
	 * Check if we already compiled this trigger procedure
	 * ----------
	 */
	for (func = compiled_functions; func != NULL; func = func->next)
	{
		if (proinfo->fn_oid == func->fn_oid)
			break;
	}

	/* ----------
	 * If not, do so and add it to the compiled ones
	 * ----------
	 */
	if (func == NULL)
	{
		func = plpgsql_compile(proinfo->fn_oid, T_TRIGGER);

		func->next = compiled_functions;
		compiled_functions = func;
	}

	return plpgsql_exec_trigger(func, trigdata);
}
