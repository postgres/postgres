/*
 * dblink.c
 *
 * Functions returning results from a remote database
 *
 * Copyright (c) Joseph Conway <mail@joeconway.com>, 2001, 2002,
 * ALL RIGHTS RESERVED;
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

/* Global */
List	*res_id = NIL;
int		res_id_index = 0;

PG_FUNCTION_INFO_V1(dblink);
Datum
dblink(PG_FUNCTION_ARGS)
{
	PGconn			*conn = NULL;
	PGresult		*res = NULL;
	dblink_results	*results;
	char			*optstr;
	char			*sqlstatement;
	char			*execstatement;
	char			*msg;
	int				ntuples = 0;
	ReturnSetInfo	*rsi;

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

		execstatement = (char *) palloc(strlen(sqlstatement) + 1);
		if (execstatement != NULL)
		{
			strcpy(execstatement, sqlstatement);
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
			ntuples = PQntuples(res);

			/*
			 * increment resource index
			 */
			res_id_index++;

			results = init_dblink_results(fcinfo->flinfo->fn_mcxt);
			results->tup_num = 0;
			results->res_id_index = res_id_index;
			results->res = res;

			/*
			 * Append node to res_id to hold pointer to results.
			 * Needed by dblink_tok to access the data
			 */
			append_res_ptr(results);

			/*
			 * save pointer to results for the next function manager call
			 */
			fcinfo->flinfo->fn_extra = (void *) results;

			/* close the connection to the database and cleanup */
			PQfinish(conn);

			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprMultipleResult;

			PG_RETURN_INT32(res_id_index);
		}
	}
	else
	{
		/*
		 * check for more results
		 */
		results = fcinfo->flinfo->fn_extra;

		results->tup_num++;
		res_id_index = results->res_id_index;
		ntuples = PQntuples(results->res);

		if (results->tup_num < ntuples)
		{
			/*
			 * fetch them if available
			 */

			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprMultipleResult;

			PG_RETURN_INT32(res_id_index);
		}
		else
		{
			/*
			 * or if no more, clean things up
			 */
			results = fcinfo->flinfo->fn_extra;

			remove_res_ptr(results);
			PQclear(results->res);
			pfree(results);
			fcinfo->flinfo->fn_extra = NULL;

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
	int				fldnum;
	text			*result_text;
	char			*result;
	int				nfields = 0;
	int				text_len = 0;

	results = get_res_ptr(PG_GETARG_INT32(0));
	if (results == NULL)
	{
		if (res_id != NIL)
		{
			freeList(res_id);
			res_id = NIL;
			res_id_index = 0;
		}

		elog(ERROR, "dblink_tok: function called with invalid resource id");
	}

	fldnum = PG_GETARG_INT32(1);
	if (fldnum < 0)
		elog(ERROR, "dblink_tok: field number < 0 not permitted");

	nfields = PQnfields(results->res);
	if (fldnum > (nfields - 1))
		elog(ERROR, "dblink_tok: field number %d does not exist", fldnum);

	if (PQgetisnull(results->res, results->tup_num, fldnum) == 1)
		PG_RETURN_NULL();
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
 * dblink_strtok
 * parse input string
 * return ord item (0 based)
 * based on provided field separator
 */
PG_FUNCTION_INFO_V1(dblink_strtok);
Datum
dblink_strtok(PG_FUNCTION_ARGS)
{
	char		*fldtext;
	char		*fldsep;
	int			fldnum;
	char		*buffer;
	text		*result_text;

	fldtext = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(PG_GETARG_TEXT_P(0))));
	fldsep = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(PG_GETARG_TEXT_P(1))));
	fldnum = PG_GETARG_INT32(2);

	if (fldtext[0] == '\0')
	{
		elog(ERROR, "get_strtok: blank list not permitted");
	}
	if (fldsep[0] == '\0')
	{
		elog(ERROR, "get_strtok: blank field separator not permitted");
	}

	buffer = get_strtok(fldtext, fldsep, fldnum);

	pfree(fldtext);
	pfree(fldsep);

	if (buffer == NULL)
	{
		PG_RETURN_NULL();
	}
	else
	{
		result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(buffer)));
		pfree(buffer);

		PG_RETURN_TEXT_P(result_text);
	}
}


/*
 * dblink_get_pkey
 * 
 * Return comma delimited list of primary key
 * fields for the supplied relation,
 * or NULL if none exists.
 */
PG_FUNCTION_INFO_V1(dblink_get_pkey);
Datum
dblink_get_pkey(PG_FUNCTION_ARGS)
{
	char					*relname;
	Oid						relid;
	char					**result;
	text					*result_text;
	int16					numatts;
	ReturnSetInfo			*rsi;
	dblink_array_results	*ret_set;

	if (fcinfo->resultinfo == NULL || !IsA(fcinfo->resultinfo, ReturnSetInfo))
		elog(ERROR, "dblink: function called in context that does not accept a set result");

	if (fcinfo->flinfo->fn_extra == NULL)
	{
		relname = NameStr(*PG_GETARG_NAME(0));

		/*
		 * Convert relname to rel OID.
		 */
		relid = get_relid_from_relname(relname);
		if (!OidIsValid(relid))
			elog(ERROR, "dblink_get_pkey: relation \"%s\" does not exist",
				 relname);

		/*
		 * get an array of attnums.
		 */
		result = get_pkey_attnames(relid, &numatts);

		if ((result != NULL) && (numatts > 0))
		{
			ret_set = init_dblink_array_results(fcinfo->flinfo->fn_mcxt);

			ret_set->elem_num = 0;
			ret_set->num_elems = numatts;
			ret_set->res = result;

			fcinfo->flinfo->fn_extra = (void *) ret_set;

			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprMultipleResult;

			result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(result[ret_set->elem_num])));

			PG_RETURN_TEXT_P(result_text);
		}
		else
		{
			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprEndResult;

			PG_RETURN_NULL();
		}
	}
	else
	{
		/*
		 * check for more results
		 */
		ret_set = fcinfo->flinfo->fn_extra;
		ret_set->elem_num++;
		result = ret_set->res;

		if (ret_set->elem_num < ret_set->num_elems)
		{
			/*
			 * fetch next one
			 */
			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprMultipleResult;

			result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(result[ret_set->elem_num])));
			PG_RETURN_TEXT_P(result_text);
		}
		else
		{
			int		i;

			/*
			 * or if no more, clean things up
			 */
			for (i = 0; i < ret_set->num_elems; i++)
				pfree(result[i]);

			pfree(ret_set->res);
			pfree(ret_set);

			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprEndResult;

			PG_RETURN_NULL();
		}
	}
	PG_RETURN_NULL();
}


/*
 * dblink_last_oid
 * return last inserted oid
 */
PG_FUNCTION_INFO_V1(dblink_last_oid);
Datum
dblink_last_oid(PG_FUNCTION_ARGS)
{
	dblink_results *results;

	results = get_res_ptr(PG_GETARG_INT32(0));
	if (results == NULL)
	{
		if (res_id != NIL)
		{
			freeList(res_id);
			res_id = NIL;
			res_id_index = 0;
		}

		elog(ERROR, "dblink_tok: function called with invalid resource id");
	}

	PG_RETURN_OID(PQoidValue(results->res));
}


/*
 * dblink_build_sql_insert
 * 
 * Used to generate an SQL insert statement
 * based on an existing tuple in a local relation.
 * This is useful for selectively replicating data
 * to another server via dblink.
 *
 * API:
 * <relname> - name of local table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the local tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <src_pkattvals_arry> - text array of key values which will be used
 * to identify the local tuple of interest
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely. These are substituted
 * for their counterparts in src_pkattvals_arry
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_insert);
Datum
dblink_build_sql_insert(PG_FUNCTION_ARGS)
{
	Oid			relid;
	char		*relname;
	int16		*pkattnums;
	int16		pknumatts;
	char		**src_pkattvals;
	char		**tgt_pkattvals;
	ArrayType	*src_pkattvals_arry;
	ArrayType	*tgt_pkattvals_arry;
	int			src_ndim;
	int			*src_dim;
	int			src_nitems;
	int			tgt_ndim;
	int			*tgt_dim;
	int			tgt_nitems;
	int			i;
	char		*ptr;
	char		*sql;
	text		*sql_text;

	relname = NameStr(*PG_GETARG_NAME(0));

	/*
	 * Convert relname to rel OID.
	 */
	relid = get_relid_from_relname(relname);
	if (!OidIsValid(relid))
		elog(ERROR, "dblink_get_pkey: relation \"%s\" does not exist",
			 relname);

	pkattnums = (int16 *) PG_GETARG_POINTER(1);
	pknumatts = PG_GETARG_INT16(2);
	/*
	 * There should be at least one key attribute
	 */
	if (pknumatts == 0)
		elog(ERROR, "dblink_build_sql_insert: number of key attributes must be > 0.");

	src_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(4);

	/*
	 * Source array is made up of key values that will be used to
	 * locate the tuple of interest from the local system.
	 */
	src_ndim = ARR_NDIM(src_pkattvals_arry);
	src_dim = ARR_DIMS(src_pkattvals_arry);
	src_nitems = ArrayGetNItems(src_ndim, src_dim);

	/*
	 * There should be one source array key value for each key attnum
	 */
	if (src_nitems != pknumatts)
		elog(ERROR, "dblink_build_sql_insert: source key array length does not match number of key attributes.");

	/*
	 * get array of pointers to c-strings from the input source array
	 */
	src_pkattvals = (char **) palloc(src_nitems * sizeof(char *));
	ptr = ARR_DATA_PTR(src_pkattvals_arry);
	for (i = 0; i < src_nitems; i++)
	{
		src_pkattvals[i] = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(ptr)));
		ptr += INTALIGN(*(int32 *) ptr);
	}

	/*
	 * Target array is made up of key values that will be used to
	 * build the SQL string for use on the remote system.
	 */
	tgt_ndim = ARR_NDIM(tgt_pkattvals_arry);
	tgt_dim = ARR_DIMS(tgt_pkattvals_arry);
	tgt_nitems = ArrayGetNItems(tgt_ndim, tgt_dim);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		elog(ERROR, "dblink_build_sql_insert: target key array length does not match number of key attributes.");

	/*
	 * get array of pointers to c-strings from the input target array
	 */
	tgt_pkattvals = (char **) palloc(tgt_nitems * sizeof(char *));
	ptr = ARR_DATA_PTR(tgt_pkattvals_arry);
	for (i = 0; i < tgt_nitems; i++)
	{
		tgt_pkattvals[i] = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(ptr)));
		ptr += INTALIGN(*(int32 *) ptr);
	}

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_insert(relid, pkattnums, pknumatts, src_pkattvals, tgt_pkattvals);

	/*
	 * Make it into TEXT for return to the client
	 */
	sql_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(sql)));

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(sql_text);
}


/*
 * dblink_build_sql_delete
 * 
 * Used to generate an SQL delete statement.
 * This is useful for selectively replicating a
 * delete to another server via dblink.
 *
 * API:
 * <relname> - name of remote table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the remote tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely.
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_delete);
Datum
dblink_build_sql_delete(PG_FUNCTION_ARGS)
{
	Oid			relid;
	char		*relname;
	int16		*pkattnums;
	int16		pknumatts;
	char		**tgt_pkattvals;
	ArrayType	*tgt_pkattvals_arry;
	int			tgt_ndim;
	int			*tgt_dim;
	int			tgt_nitems;
	int			i;
	char		*ptr;
	char		*sql;
	text		*sql_text;

	relname = NameStr(*PG_GETARG_NAME(0));

	/*
	 * Convert relname to rel OID.
	 */
	relid = get_relid_from_relname(relname);
	if (!OidIsValid(relid))
		elog(ERROR, "dblink_get_pkey: relation \"%s\" does not exist",
			 relname);

	pkattnums = (int16 *) PG_GETARG_POINTER(1);
	pknumatts = PG_GETARG_INT16(2);
	/*
	 * There should be at least one key attribute
	 */
	if (pknumatts == 0)
		elog(ERROR, "dblink_build_sql_insert: number of key attributes must be > 0.");

	tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);

	/*
	 * Target array is made up of key values that will be used to
	 * build the SQL string for use on the remote system.
	 */
	tgt_ndim = ARR_NDIM(tgt_pkattvals_arry);
	tgt_dim = ARR_DIMS(tgt_pkattvals_arry);
	tgt_nitems = ArrayGetNItems(tgt_ndim, tgt_dim);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		elog(ERROR, "dblink_build_sql_insert: target key array length does not match number of key attributes.");

	/*
	 * get array of pointers to c-strings from the input target array
	 */
	tgt_pkattvals = (char **) palloc(tgt_nitems * sizeof(char *));
	ptr = ARR_DATA_PTR(tgt_pkattvals_arry);
	for (i = 0; i < tgt_nitems; i++)
	{
		tgt_pkattvals[i] = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(ptr)));
		ptr += INTALIGN(*(int32 *) ptr);
	}

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_delete(relid, pkattnums, pknumatts, tgt_pkattvals);

	/*
	 * Make it into TEXT for return to the client
	 */
	sql_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(sql)));

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(sql_text);
}


/*
 * dblink_build_sql_update
 * 
 * Used to generate an SQL update statement
 * based on an existing tuple in a local relation.
 * This is useful for selectively replicating data
 * to another server via dblink.
 *
 * API:
 * <relname> - name of local table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the local tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <src_pkattvals_arry> - text array of key values which will be used
 * to identify the local tuple of interest
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely. These are substituted
 * for their counterparts in src_pkattvals_arry
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_update);
Datum
dblink_build_sql_update(PG_FUNCTION_ARGS)
{
	Oid			relid;
	char		*relname;
	int16		*pkattnums;
	int16		pknumatts;
	char		**src_pkattvals;
	char		**tgt_pkattvals;
	ArrayType	*src_pkattvals_arry;
	ArrayType	*tgt_pkattvals_arry;
	int			src_ndim;
	int			*src_dim;
	int			src_nitems;
	int			tgt_ndim;
	int			*tgt_dim;
	int			tgt_nitems;
	int			i;
	char		*ptr;
	char		*sql;
	text		*sql_text;

	relname = NameStr(*PG_GETARG_NAME(0));

	/*
	 * Convert relname to rel OID.
	 */
	relid = get_relid_from_relname(relname);
	if (!OidIsValid(relid))
		elog(ERROR, "dblink_get_pkey: relation \"%s\" does not exist",
			 relname);

	pkattnums = (int16 *) PG_GETARG_POINTER(1);
	pknumatts = PG_GETARG_INT16(2);
	/*
	 * There should be one source array key values for each key attnum
	 */
	if (pknumatts == 0)
		elog(ERROR, "dblink_build_sql_insert: number of key attributes must be > 0.");

	src_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(4);

	/*
	 * Source array is made up of key values that will be used to
	 * locate the tuple of interest from the local system.
	 */
	src_ndim = ARR_NDIM(src_pkattvals_arry);
	src_dim = ARR_DIMS(src_pkattvals_arry);
	src_nitems = ArrayGetNItems(src_ndim, src_dim);

	/*
	 * There should be one source array key value for each key attnum
	 */
	if (src_nitems != pknumatts)
		elog(ERROR, "dblink_build_sql_insert: source key array length does not match number of key attributes.");

	/*
	 * get array of pointers to c-strings from the input source array
	 */
	src_pkattvals = (char **) palloc(src_nitems * sizeof(char *));
	ptr = ARR_DATA_PTR(src_pkattvals_arry);
	for (i = 0; i < src_nitems; i++)
	{
		src_pkattvals[i] = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(ptr)));
		ptr += INTALIGN(*(int32 *) ptr);
	}

	/*
	 * Target array is made up of key values that will be used to
	 * build the SQL string for use on the remote system.
	 */
	tgt_ndim = ARR_NDIM(tgt_pkattvals_arry);
	tgt_dim = ARR_DIMS(tgt_pkattvals_arry);
	tgt_nitems = ArrayGetNItems(tgt_ndim, tgt_dim);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		elog(ERROR, "dblink_build_sql_insert: target key array length does not match number of key attributes.");

	/*
	 * get array of pointers to c-strings from the input target array
	 */
	tgt_pkattvals = (char **) palloc(tgt_nitems * sizeof(char *));
	ptr = ARR_DATA_PTR(tgt_pkattvals_arry);
	for (i = 0; i < tgt_nitems; i++)
	{
		tgt_pkattvals[i] = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(ptr)));
		ptr += INTALIGN(*(int32 *) ptr);
	}

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_update(relid, pkattnums, pknumatts, src_pkattvals, tgt_pkattvals);

	/*
	 * Make it into TEXT for return to the client
	 */
	sql_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(sql)));

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(sql_text);
}


/*
 * dblink_current_query
 * return the current query string
 * to allow its use in (among other things)
 * rewrite rules
 */
PG_FUNCTION_INFO_V1(dblink_current_query);
Datum
dblink_current_query(PG_FUNCTION_ARGS)
{
	text		*result_text;

	result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(debug_query_string)));
	PG_RETURN_TEXT_P(result_text);
}


/*
 * dblink_replace_text
 * replace all occurences of 'old_sub_str' in 'orig_str'
 * with 'new_sub_str' to form 'new_str'
 * 
 * returns 'orig_str' if 'old_sub_str' == '' or 'orig_str' == ''
 * otherwise returns 'new_str' 
 */
PG_FUNCTION_INFO_V1(dblink_replace_text);
Datum
dblink_replace_text(PG_FUNCTION_ARGS)
{
	text		*left_text;
	text		*right_text;
	text		*buf_text;
	text		*ret_text;
	char		*ret_str;
	int			curr_posn;
	text		*src_text = PG_GETARG_TEXT_P(0);
	int			src_text_len = DatumGetInt32(DirectFunctionCall1(textlen, PointerGetDatum(src_text)));
	text		*from_sub_text = PG_GETARG_TEXT_P(1);
	int			from_sub_text_len = DatumGetInt32(DirectFunctionCall1(textlen, PointerGetDatum(from_sub_text)));
	text		*to_sub_text = PG_GETARG_TEXT_P(2);
	char		*to_sub_str = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(to_sub_text)));
	StringInfo	str = makeStringInfo();

	if (src_text_len == 0 || from_sub_text_len == 0)
		PG_RETURN_TEXT_P(src_text);

	buf_text = DatumGetTextPCopy(PointerGetDatum(src_text));
	curr_posn = DatumGetInt32(DirectFunctionCall2(textpos, PointerGetDatum(buf_text), PointerGetDatum(from_sub_text)));

	while (curr_posn > 0)
	{
		left_text = DatumGetTextP(DirectFunctionCall3(text_substr, PointerGetDatum(buf_text), 1, DatumGetInt32(DirectFunctionCall2(textpos, PointerGetDatum(buf_text), PointerGetDatum(from_sub_text))) - 1));
		right_text = DatumGetTextP(DirectFunctionCall3(text_substr, PointerGetDatum(buf_text), DatumGetInt32(DirectFunctionCall2(textpos, PointerGetDatum(buf_text), PointerGetDatum(from_sub_text))) + from_sub_text_len, -1));

		appendStringInfo(str, DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(left_text))));
		appendStringInfo(str, to_sub_str);

		pfree(buf_text);
		pfree(left_text);
		buf_text = right_text;
		curr_posn = DatumGetInt32(DirectFunctionCall2(textpos, PointerGetDatum(buf_text), PointerGetDatum(from_sub_text)));
	}

	appendStringInfo(str, DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(buf_text))));
	pfree(buf_text);

	ret_str = pstrdup(str->data);
	ret_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(ret_str)));

	PG_RETURN_TEXT_P(ret_text);
}


/*************************************************************
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
	retval->res_id_index =-1;
	retval->res = NULL;

	MemoryContextSwitchTo(oldcontext);

	return retval;
}


/*
 * init_dblink_array_results
 *	 - create an empty dblink_array_results data structure
 */
dblink_array_results *
init_dblink_array_results(MemoryContext fn_mcxt)
{
	MemoryContext oldcontext;
	dblink_array_results *retval;

	oldcontext = MemoryContextSwitchTo(fn_mcxt);

	retval = (dblink_array_results *) palloc(sizeof(dblink_array_results));
	MemSet(retval, 0, sizeof(dblink_array_results));

	retval->elem_num = -1;
	retval->num_elems = 0;
	retval->res = NULL;

	MemoryContextSwitchTo(oldcontext);

	return retval;
}

/*
 * get_pkey_attnames
 * 
 * Get the primary key attnames for the given relation.
 * Return NULL, and set numatts = 0, if no primary key exists.
 */
char **
get_pkey_attnames(Oid relid, int16 *numatts)
{
	Relation		indexRelation;
	ScanKeyData		entry;
	HeapScanDesc	scan;
	HeapTuple		indexTuple;
	int				i;
	char			**result = NULL;
	Relation		rel;
	TupleDesc		tupdesc;

	/*
	 * Open relation using relid, get tupdesc
	 */
	rel = relation_open(relid, AccessShareLock);
	tupdesc = rel->rd_att;

	/*
	 * Initialize numatts to 0 in case no primary key
	 * exists
	 */
	*numatts = 0;

	/*
	 * Use relid to get all related indexes
	 */
	indexRelation = heap_openr(IndexRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_index_indrelid,
						   F_OIDEQ, ObjectIdGetDatum(relid));
	scan = heap_beginscan(indexRelation, SnapshotNow, 1, &entry);

	while ((indexTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_index	index = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * We're only interested if it is the primary key
		 */
		if (index->indisprimary == TRUE)
		{
			i = 0;
			while (index->indkey[i++] != 0)
				(*numatts)++;

			if (*numatts > 0)
			{
				result = (char **) palloc(*numatts * sizeof(char *));
				for (i = 0; i < *numatts; i++)
					result[i] = SPI_fname(tupdesc, index->indkey[i]);
			}
			break;
		}
	}
	heap_endscan(scan);
	heap_close(indexRelation, AccessShareLock);
	relation_close(rel, AccessShareLock);

	return result;
}


/*
 * get_strtok
 * 
 * parse input string
 * return ord item (0 based)
 * based on provided field separator
 */
char *
get_strtok(char *fldtext, char *fldsep, int fldnum)
{
	int			j = 0;
	char		*result;

	if (fldnum < 0)
	{
		elog(ERROR, "get_strtok: field number < 0 not permitted");
	}

	if (fldsep[0] == '\0')
	{
		elog(ERROR, "get_strtok: blank field separator not permitted");
	}

	result = strtok(fldtext, fldsep);
	for (j = 1; j < fldnum + 1; j++)
	{
		result = strtok(NULL, fldsep);
		if (result == NULL)
			return NULL;
	} 

	return pstrdup(result);
}

char *
get_sql_insert(Oid relid, int16 *pkattnums, int16 pknumatts, char **src_pkattvals, char **tgt_pkattvals)
{
	Relation		rel;
	char			*relname;
	HeapTuple		tuple;
	TupleDesc		tupdesc;
	int				natts;
	StringInfo		str = makeStringInfo();
	char			*sql = NULL;
	char			*val = NULL;
	int16			key;
	unsigned int	i;

	/*
	 * Open relation using relid
	 */
	rel = relation_open(relid, AccessShareLock);
	relname =  RelationGetRelationName(rel);
	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	tuple = get_tuple_of_interest(relid, pkattnums, pknumatts, src_pkattvals);

	appendStringInfo(str, "INSERT INTO %s(", quote_ident_cstr(relname));
	for (i = 0; i < natts; i++)
	{
		if (i > 0)
			appendStringInfo(str, ",");

		appendStringInfo(str, NameStr(tupdesc->attrs[i]->attname));
	}

	appendStringInfo(str, ") VALUES(");

	/*
	 * remember attvals are 1 based
	 */
	for (i = 0; i < natts; i++)
	{
		if (i > 0)
			appendStringInfo(str, ",");

		if (tgt_pkattvals != NULL)
			key = get_attnum_pk_pos(pkattnums, pknumatts, i + 1);
		else
			key = -1;

		if (key > -1)
			val = pstrdup(tgt_pkattvals[key]);
		else
			val = SPI_getvalue(tuple, tupdesc, i + 1);

		if (val != NULL)
		{
			appendStringInfo(str, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(str, "NULL");
	}
	appendStringInfo(str, ")");

	sql = pstrdup(str->data);
	pfree(str->data);
	pfree(str);
	relation_close(rel, AccessShareLock);

	return (sql);
}

char *
get_sql_delete(Oid relid, int16 *pkattnums, int16 pknumatts, char **tgt_pkattvals)
{
	Relation		rel;
	char			*relname;
	TupleDesc		tupdesc;
	int				natts;
	StringInfo		str = makeStringInfo();
	char			*sql = NULL;
	char			*val = NULL;
	unsigned int	i;

	/*
	 * Open relation using relid
	 */
	rel = relation_open(relid, AccessShareLock);
	relname =  RelationGetRelationName(rel);
	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	appendStringInfo(str, "DELETE FROM %s WHERE ", quote_ident_cstr(relname));
	for (i = 0; i < pknumatts; i++)
	{
		int16	pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfo(str, " AND ");

		appendStringInfo(str, NameStr(tupdesc->attrs[pkattnum - 1]->attname));

		if (tgt_pkattvals != NULL)
			val = pstrdup(tgt_pkattvals[i]);
		else
			elog(ERROR, "Target key array must not be NULL");

		if (val != NULL)
		{
			appendStringInfo(str, "=");
			appendStringInfo(str, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(str, "IS NULL");
	}

	sql = pstrdup(str->data);
	pfree(str->data);
	pfree(str);
	relation_close(rel, AccessShareLock);

	return (sql);
}

char *
get_sql_update(Oid relid, int16 *pkattnums, int16 pknumatts, char **src_pkattvals, char **tgt_pkattvals)
{
	Relation		rel;
	char			*relname;
	HeapTuple		tuple;
	TupleDesc		tupdesc;
	int				natts;
	StringInfo		str = makeStringInfo();
	char			*sql = NULL;
	char			*val = NULL;
	int16			key;
	int				i;

	/*
	 * Open relation using relid
	 */
	rel = relation_open(relid, AccessShareLock);
	relname =  RelationGetRelationName(rel);
	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	tuple = get_tuple_of_interest(relid, pkattnums, pknumatts, src_pkattvals);

	appendStringInfo(str, "UPDATE %s SET ", quote_ident_cstr(relname));

	for (i = 0; i < natts; i++)
	{
		if (i > 0)
			appendStringInfo(str, ",");

		appendStringInfo(str, NameStr(tupdesc->attrs[i]->attname));
		appendStringInfo(str, "=");

		if (tgt_pkattvals != NULL)
			key = get_attnum_pk_pos(pkattnums, pknumatts, i + 1);
		else
			key = -1;

		if (key > -1)
			val = pstrdup(tgt_pkattvals[key]);
		else
			val = SPI_getvalue(tuple, tupdesc, i + 1);

		if (val != NULL)
		{
			appendStringInfo(str, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(str, "NULL");
	}

	appendStringInfo(str, " WHERE ");

	for (i = 0; i < pknumatts; i++)
	{
		int16	pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfo(str, " AND ");

		appendStringInfo(str, NameStr(tupdesc->attrs[pkattnum - 1]->attname));

		if (tgt_pkattvals != NULL)
			val = pstrdup(tgt_pkattvals[i]);
		else
			val = SPI_getvalue(tuple, tupdesc, pkattnum);

		if (val != NULL)
		{
			appendStringInfo(str, "=");
			appendStringInfo(str, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(str, "IS NULL");
	}

	sql = pstrdup(str->data);
	pfree(str->data);
	pfree(str);
	relation_close(rel, AccessShareLock);

	return (sql);
}

/*
 * Return a properly quoted literal value.
 * Uses quote_literal in quote.c
 */
static char *
quote_literal_cstr(char *rawstr)
{
	text		*rawstr_text;
	text		*result_text;
	char		*result;

	rawstr_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(rawstr)));
	result_text = DatumGetTextP(DirectFunctionCall1(quote_literal, PointerGetDatum(rawstr_text)));
	result = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(result_text)));

	return result;
}

/*
 * Return a properly quoted identifier.
 * Uses quote_ident in quote.c
 */
static char *
quote_ident_cstr(char *rawstr)
{
	text		*rawstr_text;
	text		*result_text;
	char		*result;

	rawstr_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(rawstr)));
	result_text = DatumGetTextP(DirectFunctionCall1(quote_ident, PointerGetDatum(rawstr_text)));
	result = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(result_text)));

	return result;
}

int16
get_attnum_pk_pos(int16 *pkattnums, int16 pknumatts, int16 key)
{
	int		i;

	/*
	 * Not likely a long list anyway, so just scan for
	 * the value
	 */
	for (i = 0; i < pknumatts; i++)
		if (key == pkattnums[i])
			return i;

	return -1;
}

HeapTuple
get_tuple_of_interest(Oid relid, int16 *pkattnums, int16 pknumatts, char **src_pkattvals)
{
	Relation		rel;
	char			*relname;
	TupleDesc		tupdesc;
	StringInfo		str = makeStringInfo();
	char			*sql = NULL;
	int				ret;
	HeapTuple		tuple;
	int				i;
	char			*val = NULL;

	/*
	 * Open relation using relid
	 */
	rel = relation_open(relid, AccessShareLock);
	relname =  RelationGetRelationName(rel);
	tupdesc = rel->rd_att;

	/*
	 * Connect to SPI manager
	 */
	if ((ret = SPI_connect()) < 0)
		elog(ERROR, "get_tuple_of_interest: SPI_connect returned %d", ret);

	/*
	 * Build sql statement to look up tuple of interest
	 * Use src_pkattvals as the criteria.
	 */
	appendStringInfo(str, "SELECT * from %s WHERE ", relname);

	for (i = 0; i < pknumatts; i++)
	{
		int16	pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfo(str, " AND ");

		appendStringInfo(str, NameStr(tupdesc->attrs[pkattnum - 1]->attname));

		val = pstrdup(src_pkattvals[i]);
		if (val != NULL)
		{
			appendStringInfo(str, "=");
			appendStringInfo(str, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(str, "IS NULL");
	}

	sql = pstrdup(str->data);
	pfree(str->data);
	pfree(str);
	/*
	 * Retrieve the desired tuple
	 */
	ret = SPI_exec(sql, 0);
	pfree(sql);

	/*
	 * Only allow one qualifying tuple
	 */
	if ((ret == SPI_OK_SELECT) && (SPI_processed > 1))
	{
		elog(ERROR, "get_tuple_of_interest: Source criteria may not match more than one record.");
	}
	else if (ret == SPI_OK_SELECT && SPI_processed == 1)
	{
		SPITupleTable *tuptable = SPI_tuptable;
		tuple = SPI_copytuple(tuptable->vals[0]);

		return tuple;
	}
	else
	{
		/*
		 * no qualifying tuples
		 */
		return NULL;
	}

	/*
	 * never reached, but keep compiler quiet
	 */
	return NULL;
}

Oid
get_relid_from_relname(char *relname)
{
#ifdef NamespaceRelationName
	Oid				relid;

	relid = RelnameGetRelid(relname);
#else
	Relation		rel;
	Oid				relid;

	rel = relation_openr(relname, AccessShareLock);
	relid = RelationGetRelid(rel);
	relation_close(rel, AccessShareLock);
#endif   /* NamespaceRelationName */

	return relid;
}

dblink_results	*
get_res_ptr(int32 res_id_index)
{
	List	*ptr;

	/*
	 * short circuit empty list
	 */
	if(res_id == NIL)
		return NULL;

	/*
	 * OK, should be good to go
	 */
	foreach(ptr, res_id)
	{
		dblink_results	*this_res_id = (dblink_results *) lfirst(ptr);
		if (this_res_id->res_id_index == res_id_index)
			return this_res_id;
	}
	return NULL;
}

/*
 * Add node to global List res_id
 */
void
append_res_ptr(dblink_results *results)
{
	res_id = lappend(res_id, results);
}

/*
 * Remove node from global List
 * using res_id_index
 */
void
remove_res_ptr(dblink_results *results)
{
	res_id = lremove(results, res_id);

	if (res_id == NIL)
		res_id_index = 0;
}


