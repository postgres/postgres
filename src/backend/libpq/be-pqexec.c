/*-------------------------------------------------------------------------
 *
 * be-pqexec.c--
 *	  support for executing POSTGRES commands and functions from a
 *	  user-defined function in a backend.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/Attic/be-pqexec.c,v 1.17 1998/06/15 19:28:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		PQfn			- call a POSTGRES function
 *		PQexec			- execute a POSTGRES query
 *
 * NOTES
 *		These routines are compiled into the postgres backend.
 */
#include <postgres.h>

#include <nodes/pg_list.h>
#include <tcop/dest.h>
#include <tcop/fastpath.h>
#include <tcop/tcopprot.h>
#include <lib/dllist.h>
#include <libpq/libpq.h>
#include <fmgr.h>
#include <utils/exc.h>
#include <utils/builtins.h>
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

static char *strmake(char *str, int len);

/* ----------------------------------------------------------------
 *						PQ interface routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *		PQfn -	Send a function call to the POSTGRES backend.
 *
 *		fnid			: function id
 *		result_buf		: pointer to result buffer (&int if integer)
 *		result_len		: length of return value.
 *		result_is_int	: If the result is an integer, this must be non-zero
 *		args			: pointer to a NULL terminated arg array.
 *						  (length, if integer, and result-pointer)
 *		nargs			: # of arguments in args array.
 *
 *		This code scavanged from HandleFunctionRequest() in tcop/fastpath.h
 * ----------------
 */
char *
PQfn(int fnid,
	 int *result_buf,			/* can't use void, dec compiler barfs */
	 int result_len,
	 int result_is_int,
	 PQArgBlock *args,
	 int nargs)
{
	char	   *retval;			/* XXX - should be datum, maybe ? */
	char	   *arg[8];
	int			i;

	/* ----------------
	 *	fill args[] array
	 * ----------------
	 */
	for (i = 0; i < nargs; i++)
	{
		if (args[i].len == VAR_LENGTH_ARG)
			arg[i] = (char *) args[i].u.ptr;
		else if (args[i].len > sizeof(int4))
			elog(ERROR, "arg_length of argument %d too long", i);
		else
			arg[i] = (char *) args[i].u.integer;
	}

	/* ----------------
	 *	call the postgres function manager
	 * ----------------
	 */
	retval = (char *)
		fmgr(fnid, arg[0], arg[1], arg[2], arg[3],
			 arg[4], arg[5], arg[6], arg[7]);

	/* ----------------
	 *	put the result in the buffer the user specified and
	 *	return the proper code.
	 * ----------------
	 */
	if (retval == (char *) NULL)/* void retval */
		return "0";

	if (result_is_int)
		*result_buf = (int) retval;
	else
		memmove(result_buf, retval, result_len);
	return "G";
}

/* ----------------
 *		PQexec -  Send a query to the POSTGRES backend
 *
 *		The return value is a string.
 *		If 0 or more tuples fetched from the backend, return "P portal-name".
 *		If a query is does not return tuples, return "C query-command".
 *		If there is an error: return "E error-message".
 *
 *		Note: if we get a serious error or an elog(ERROR), then PQexec never
 *		returns because the system longjmp's back to the main loop.
 * ----------------
 */
char *
PQexec(char *query)
{
	PortalEntry *entry = NULL;
	char	   *result = NULL;

	/* ----------------
	 *	create a new portal and put it on top of the portal stack.
	 * ----------------
	 */
	entry = (PortalEntry *) be_newportal();
	be_portalpush(entry);

	/* ----------------
	 *	pg_exec_query_dest will put the query results in a portal which will
	 *	end up on the top of the portal stack.
	 * ----------------
	 */
	pg_exec_query_dest(query, Local);

	/* ----------------
	 *	pop the portal off the portal stack and return the
	 *	result.  Note if result is null, we return C.
	 * ----------------
	 */
	entry = (PortalEntry *) be_portalpop();
	result = entry->result;
	if (result == NULL)
	{
		char	   *PQE = "Cnull PQexec result";

		result = pstrdup(PQE);
	}

	if (result[0] != 'P')
	{

		/*
		 * some successful command was executed, but it's not one where we
		 * return the portal name so here we should be sure to clear out
		 * the portal (since the caller has no handle on it)
		 */
		pbuf_close(entry->name);

	}
	return result;
}

/* ----------------------------------------------------------------
 *						pqtest support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		pqtest_PQexec takes a text query and returns the number of
 *		tuples it returns.	Note: there is no need to PQclear()
 *		here - the memory will go away at end transaction.
 * ----------------
 */
int
pqtest_PQexec(char *q)
{
	PortalBuffer *a;
	char	   *res;
	int			t;

	/* ----------------
	 *	execute the postgres query
	 * ----------------
	 */
	res = PQexec(q);

	/* ----------------
	 *	return number of tuples in portal or 0 if command returns no tuples.
	 * ----------------
	 */
	t = 0;
	switch (res[0])
	{
		case 'P':
			a = PQparray(&res[1]);
			if (a == NULL)
				elog(ERROR, "pqtest_PQexec: PQparray could not find portal %s",
					 res);

			t = PQntuples(a);
			break;
		case 'C':
			break;
		default:
			elog(NOTICE, "pqtest_PQexec: PQexec(%s) returns %s", q, res);
			break;
	}

	return t;
}

/* ----------------
 *		utilities for pqtest_PQfn()
 * ----------------
 */
static char *
strmake(char *str, int len)
{
	char	   *newstr;

	if (str == NULL)
		return NULL;
	if (len <= 0)
		len = strlen(str);

	newstr = (char *) palloc((unsigned) len + 1);
	StrNCpy(newstr, str, len + 1);
	newstr[len] = (char) 0;
	return newstr;
}

#define SKIP 0
#define SCAN 1

static char spacestr[] = " ";

static int
strparse(char *s, char **fields, int *offsets, int maxfields)
{
	int			len = strlen(s);
	char	   *cp = s,
			   *end = cp + len,
			   *ep;
	int			parsed = 0;
	int			mode = SKIP,
				i = 0;

	if (*(end - 1) == '\n')
		end--;

	for (i = 0; i < maxfields; i++)
		fields[i] = spacestr;

	i = 0;
	while (!parsed)
	{
		if (mode == SKIP)
		{

			while ((cp < end) &&
				   (*cp == ' ' || *cp == '\t'))
				cp++;
			if (cp < end)
				mode = SCAN;
			else
				parsed = 1;

		}
		else
		{

			ep = cp;
			while ((ep < end) && (*ep != ' ' && *ep != '\t'))
				ep++;

			if (ep < end)
				mode = SKIP;
			else
				parsed = 1;

			fields[i] = strmake(cp, ep - cp);
			if (offsets != NULL)
				offsets[i] = cp - s;

			i++;
			cp = ep;
			if (i > maxfields)
				parsed = 1;

		}
	}
	return i;
}

/* ----------------
 *		pqtest_PQfn converts it's string into a PQArgBlock and
 *		calls the specified function, which is assumed to return
 *		an integer value.
 * ----------------
 */
int
pqtest_PQfn(char *q)
{
	int			k,
				j,
				i,
				v,
				f,
				offsets;
	char	   *fields[8];
	PQArgBlock	pqargs[7];
	int			res;
	char	   *pqres;

	/* ----------------
	 *	parse q into fields
	 * ----------------
	 */
	i = strparse(q, fields, &offsets, 8);
	printf("pqtest_PQfn: strparse returns %d fields\n", i);		/* debug */
	if (i == 0)
		return -1;

	/* ----------------
	 *	get the function id
	 * ----------------
	 */
	f = atoi(fields[0]);
	printf("pqtest_PQfn: func is %d\n", f);		/* debug */
	if (f == 0)
		return -1;

	/* ----------------
	 *	build a PQArgBlock
	 * ----------------
	 */
	for (j = 1; j < i && j < 8; j++)
	{
		k = j - 1;
		v = atoi(fields[j]);
		if (v != 0 || (v == 0 && fields[j][0] == '0'))
		{
			pqargs[k].len = sizeof(int4);
			pqargs[k].u.integer = v;
			printf("pqtest_PQfn: arg %d is int %d\n", k, v);	/* debug */
		}
		else
		{
			pqargs[k].len = VAR_LENGTH_ARG;
			pqargs[k].u.ptr = (int *) textin(fields[j]);
			printf("pqtest_PQfn: arg %d is text %s\n", k, fields[j]);	/* debug */
		}
	}

	/* ----------------
	 *	call PQfn
	 * ----------------
	 */
	pqres = PQfn(f, &res, sizeof(int4), 1, pqargs, i - 1);
	printf("pqtest_PQfn: pqres is %s\n", pqres);		/* debug */

	/* ----------------
	 *	free memory used
	 * ----------------
	 */
	for (j = 0; j < i; j++)
	{
		pfree(fields[j]);
		if (pqargs[j].len == VAR_LENGTH_ARG)
			pfree(pqargs[j].u.ptr);
	}

	/* ----------------
	 *	return result
	 * ----------------
	 */
	printf("pqtest_PQfn: res is %d\n", res);	/* debugg */
	return res;
}

/* ----------------
 *		pqtest looks at the first character of it's test argument
 *		and decides which of pqtest_PQexec or pqtest_PQfn to call.
 * ----------------
 */
int32
pqtest(struct varlena * vlena)
{
	char	   *q;

	/* ----------------
	 *	get the query
	 * ----------------
	 */
	q = textout(vlena);
	if (q == NULL)
		return -1;

	switch (q[0])
	{
		case '%':
			return pqtest_PQfn(&q[1]);
			break;
		default:
			return pqtest_PQexec(q);
			break;
	}
	return (0);
}
