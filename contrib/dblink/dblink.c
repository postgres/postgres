/*
 * dblink.c
 *
 * Functions returning results from a remote database
 *
 * Copyright (c) Joseph Conway <joe.conway@mail.com>, 2001;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */

#include "dblink.h"

PG_FUNCTION_INFO_V1(dblink);
Datum
dblink(PG_FUNCTION_ARGS)
{
	PGconn	   *conn = NULL;
	PGresult   *res = NULL;
	dblink_results *results;
	char	   *optstr;
	char	   *sqlstatement;
	char	   *curstr = "DECLARE mycursor CURSOR FOR ";
	char	   *execstatement;
	char	   *msg;
	int			ntuples = 0;
	ReturnSetInfo *rsi;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		elog(ERROR, "dblink: NULL arguments are not permitted");

	if (fcinfo->resultinfo == NULL || !IsA(fcinfo->resultinfo, ReturnSetInfo))
		elog(ERROR, "dblink: function called in context that does not accept a set result");

	optstr = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(PG_GETARG_TEXT_P(0))));
	sqlstatement = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(PG_GETARG_TEXT_P(1))));

	if (fcinfo->flinfo->fn_extra == NULL)
	{

		conn = PQconnectdb(optstr);
		if (PQstatus(conn) == CONNECTION_BAD)
		{
			msg = pstrdup(PQerrorMessage(conn));
			PQfinish(conn);
			elog(ERROR, "dblink: connection error: %s", msg);
		}

		res = PQexec(conn, "BEGIN");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			msg = pstrdup(PQerrorMessage(conn));
			PQclear(res);
			PQfinish(conn);
			elog(ERROR, "dblink: begin error: %s", msg);
		}
		PQclear(res);

		execstatement = (char *) palloc(strlen(curstr) + strlen(sqlstatement) + 1);
		if (execstatement != NULL)
		{
			strcpy(execstatement, curstr);
			strcat(execstatement, sqlstatement);
			strcat(execstatement, "\0");
		}
		else
			elog(ERROR, "dblink: insufficient memory");

		res = PQexec(conn, execstatement);
		if (!res || (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK))
		{
			msg = pstrdup(PQerrorMessage(conn));
			PQclear(res);
			PQfinish(conn);
			elog(ERROR, "dblink: sql error: %s", msg);
		}
		else
		{
			/*
			 * got results, start fetching them
			 */
			PQclear(res);

			res = PQexec(conn, "FETCH ALL in mycursor");
			if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				msg = pstrdup(PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				elog(ERROR, "dblink: sql error: %s", msg);
			}

			ntuples = PQntuples(res);

			if (ntuples > 0)
			{

				results = init_dblink_results(fcinfo->flinfo->fn_mcxt);
				results->tup_num = 0;
				results->res = res;
				res = NULL;

				fcinfo->flinfo->fn_extra = (void *) results;

				results = NULL;
				results = fcinfo->flinfo->fn_extra;

				/* close the cursor */
				res = PQexec(conn, "CLOSE mycursor");
				PQclear(res);

				/* commit the transaction */
				res = PQexec(conn, "COMMIT");
				PQclear(res);

				/* close the connection to the database and cleanup */
				PQfinish(conn);

				rsi = (ReturnSetInfo *) fcinfo->resultinfo;
				rsi->isDone = ExprMultipleResult;

				PG_RETURN_POINTER(results);

			}
			else
			{

				PQclear(res);

				/* close the cursor */
				res = PQexec(conn, "CLOSE mycursor");
				PQclear(res);

				/* commit the transaction */
				res = PQexec(conn, "COMMIT");
				PQclear(res);

				/* close the connection to the database and cleanup */
				PQfinish(conn);

				rsi = (ReturnSetInfo *) fcinfo->resultinfo;
				rsi->isDone = ExprEndResult;

				PG_RETURN_NULL();
			}
		}
	}
	else
	{
		/*
		 * check for more results
		 */

		results = fcinfo->flinfo->fn_extra;
		results->tup_num++;
		ntuples = PQntuples(results->res);

		if (results->tup_num < ntuples)
		{
			/*
			 * fetch them if available
			 */

			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprMultipleResult;

			PG_RETURN_POINTER(results);

		}
		else
		{
			/*
			 * or if no more, clean things up
			 */

			results = fcinfo->flinfo->fn_extra;

			PQclear(results->res);

			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprEndResult;

			PG_RETURN_NULL();
		}
	}
	PG_RETURN_NULL();
}


/*
 * dblink_tok
 * parse dblink output string
 * return fldnum item (0 based)
 * based on provided field separator
 */

PG_FUNCTION_INFO_V1(dblink_tok);
Datum
dblink_tok(PG_FUNCTION_ARGS)
{
	dblink_results *results;
	int			fldnum;
	text	   *result_text;
	char	   *result;
	int			nfields = 0;
	int			text_len = 0;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		elog(ERROR, "dblink: NULL arguments are not permitted");

	results = (dblink_results *) PG_GETARG_POINTER(0);
	if (results == NULL)
		elog(ERROR, "dblink: function called with invalid result pointer");

	fldnum = PG_GETARG_INT32(1);
	if (fldnum < 0)
		elog(ERROR, "dblink: field number < 0 not permitted");

	nfields = PQnfields(results->res);
	if (fldnum > (nfields - 1))
		elog(ERROR, "dblink: field number %d does not exist", fldnum);

	if (PQgetisnull(results->res, results->tup_num, fldnum) == 1)
	{

		PG_RETURN_NULL();

	}
	else
	{

		text_len = PQgetlength(results->res, results->tup_num, fldnum);

		result = (char *) palloc(text_len + 1);

		if (result != NULL)
		{
			strcpy(result, PQgetvalue(results->res, results->tup_num, fldnum));
			strcat(result, "\0");
		}
		else
			elog(ERROR, "dblink: insufficient memory");

		result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(result)));

		PG_RETURN_TEXT_P(result_text);

	}
}


/*
 * internal functions
 */


/*
 * init_dblink_results
 *	 - create an empty dblink_results data structure
 */
dblink_results *
init_dblink_results(MemoryContext fn_mcxt)
{
	MemoryContext oldcontext;
	dblink_results *retval;

	oldcontext = MemoryContextSwitchTo(fn_mcxt);

	retval = (dblink_results *) palloc(sizeof(dblink_results));
	MemSet(retval, 0, sizeof(dblink_results));

	retval->tup_num = -1;
	retval->res = NULL;

	MemoryContextSwitchTo(oldcontext);

	return retval;
}
