/**********************************************************************
 * pl_handler.c		- Handler for the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/pl/plpgsql/src/pl_handler.c,v 1.18 2003/09/28 23:37:45 tgl Exp $
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

#include "plpgsql.h"
#include "pl.tab.h"

#include "access/heapam.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

static int	plpgsql_firstcall = 1;

static void plpgsql_init_all(void);


/*
 * plpgsql_init()			- postmaster-startup safe initialization
 *
 * DO NOT make this static --- it has to be callable by preload
 */
void
plpgsql_init(void)
{
	/* Do initialization only once */
	if (!plpgsql_firstcall)
		return;

	plpgsql_HashTableInit();

	RegisterEOXactCallback(plpgsql_eoxact, NULL);

	plpgsql_firstcall = 0;
}

/*
 * plpgsql_init_all()		- Initialize all
 */
static void
plpgsql_init_all(void)
{
	/* Execute any postmaster-startup safe initialization */
	if (plpgsql_firstcall)
		plpgsql_init();

	/*
	 * Any other initialization that must be done each time a new backend
	 * starts -- currently none
	 */

}

/* ----------
 * plpgsql_call_handler
 *
 * This is the only visible function of the PL interpreter.
 * The PostgreSQL function manager and trigger manager
 * call this function for execution of PL/pgSQL procedures.
 * ----------
 */
PG_FUNCTION_INFO_V1(plpgsql_call_handler);

Datum
plpgsql_call_handler(PG_FUNCTION_ARGS)
{
	PLpgSQL_function *func;
	Datum		retval;

	/* perform initialization */
	plpgsql_init_all();

	/*
	 * Connect to SPI manager
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/* Find or compile the function */
	func = plpgsql_compile(fcinfo);

	/*
	 * Determine if called as function or trigger and call appropriate
	 * subhandler
	 */
	if (CALLED_AS_TRIGGER(fcinfo))
		retval = PointerGetDatum(plpgsql_exec_trigger(func,
									   (TriggerData *) fcinfo->context));
	else
		retval = plpgsql_exec_function(func, fcinfo);

	/*
	 * Disconnect from SPI manager
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	return retval;
}
