/*-------------------------------------------------------------------------
 *
 * pgtclCmds.c--
 *	  C functions which implement pg_* tcl commands
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/pgtclCmds.c,v 1.35 1998/09/21 01:02:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "postgres.h"
#include "pgtclCmds.h"
#include "pgtclId.h"
#include "libpq/libpq-fs.h"		/* large-object interface */

#ifdef TCL_ARRAYS

#define ISOCTAL(c)		(((c) >= '0') && ((c) <= '7'))
#define DIGIT(c)		((c) - '0')

/*
 * translate_escape() --
 *
 * This function performs in-place translation of a single C-style
 * escape sequence pointed by p. Curly braces { } and double-quote
 * are left escaped if they appear inside an array.
 * The value returned is the pointer to the last character (the one
 * just before the rest of the buffer).
 */

static inline char *
translate_escape(char *p, int isArray)
{
	char		c,
			   *q,
			   *s;

#ifdef TCL_ARRAYS_DEBUG_ESCAPE
	printf("   escape = '%s'\n", p);
#endif
	/* Address of the first character after the escape sequence */
	s = p + 2;
	switch (c = *(p + 1))
	{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
			c = DIGIT(c);
			if (ISOCTAL(*s))
				c = (c << 3) + DIGIT(*s++);
			if (ISOCTAL(*s))
				c = (c << 3) + DIGIT(*s++);
			*p = c;
			break;
		case 'b':
			*p = '\b';
			break;
		case 'f':
			*p = '\f';
			break;
		case 'n':
			*p = '\n';
			break;
		case 'r':
			*p = '\r';
			break;
		case 't':
			*p = '\t';
			break;
		case 'v':
			*p = '\v';
			break;
		case '\\':
		case '{':
		case '}':
		case '"':

			/*
			 * Backslahes, curly braces and double-quotes are left escaped
			 * if they appear inside an array. They will be unescaped by
			 * Tcl in Tcl_AppendElement. The buffer position is advanced
			 * by 1 so that the this character is not processed again by
			 * the caller.
			 */
			if (isArray)
				return p + 1;
			else
				*p = c;
			break;
		case '\0':

			/*
			 * This means a backslash at the end of the string. It should
			 * never happen but in that case replace the \ with a \0 but
			 * don't shift the rest of the buffer so that the caller can
			 * see the end of the string and terminate.
			 */
			*p = c;
			return p;
			break;
		default:

			/*
			 * Default case, store the escaped character over the
			 * backslash and shift the buffer over itself.
			 */
			*p = c;
	}
	/* Shift the rest of the buffer over itself after the current char */
	q = p + 1;
	for (; *s;)
		*q++ = *s++;
	*q = '\0';
#ifdef TCL_ARRAYS_DEBUG_ESCAPE
	printf("   after  = '%s'\n", p);
#endif
	return p;
}

/*
 * tcl_value() --
 *
 * This function does in-line conversion of a value returned by libpq
 * into a tcl string or into a tcl list if the value looks like the
 * representation of a postgres array.
 */

static char *
tcl_value(char *value)
{
	int			literal,
				last;
	char	   *p;

	if (!value)
		return (char *) NULL;

#ifdef TCL_ARRAYS_DEBUG
	printf("pq_value  = '%s'\n", value);
#endif
	last = strlen(value) - 1;
	if ((last >= 1) && (value[0] == '{') && (value[last] == '}'))
	{
		/* Looks like an array, replace ',' with spaces */
		/* Remove the outer pair of { }, the last first! */
		value[last] = '\0';
		value++;
		literal = 0;
		for (p = value; *p; p++)
		{
			if (!literal)
			{
				/* We are at the list level, look for ',' and '"' */
				switch (*p)
				{
					case '"':	/* beginning of literal */
						literal = 1;
						break;
					case ',':	/* replace the ',' with space */
						*p = ' ';
						break;
				}
			}
			else
			{
				/* We are inside a C string */
				switch (*p)
				{
					case '"':	/* end of literal */
						literal = 0;
						break;
					case '\\':

						/*
						 * escape sequence, translate it
						 */
						p = translate_escape(p, 1);
						break;
				}
			}
			if (!*p)
				break;
		}
	}
	else
	{
		/* Looks like a normal scalar value */
		for (p = value; *p; p++)
		{
			if (*p == '\\')
			{

				/*
				 * escape sequence, translate it
				 */
				p = translate_escape(p, 0);
			}
			if (!*p)
				break;
		}
	}
#ifdef TCL_ARRAYS_DEBUG
	printf("tcl_value = '%s'\n\n", value);
#endif
	return value;
}

#endif						/* TCL_ARRAYS */


/**********************************
 * pg_conndefaults

 syntax:
 pg_conndefaults

 the return result is a list describing the possible options and their
 current default values for a call to pg_connect with the new -conninfo
 syntax. Each entry in the list is a sublist of the format:

	 {optname label dispchar dispsize value}

 **********************************/

int
Pg_conndefaults(ClientData cData, Tcl_Interp * interp, int argc, char **argv)
{
	PQconninfoOption *option;
	char		buf[8192];

	Tcl_ResetResult(interp);
	for (option = PQconndefaults(); option->keyword != NULL; option++)
	{
		if (option->val == NULL)
			option->val = "";
		sprintf(buf, "{%s} {%s} {%s} %d {%s}",
				option->keyword,
				option->label,
				option->dispchar,
				option->dispsize,
				option->val);
		Tcl_AppendElement(interp, buf);
	}

	return TCL_OK;
}


/**********************************
 * pg_connect
 make a connection to a backend.

 syntax:
 pg_connect dbName [-host hostName] [-port portNumber] [-tty pqtty]]

 the return result is either an error message or a handle for a database
 connection.  Handles start with the prefix "pgp"

 **********************************/

int
Pg_connect(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	char	   *pghost = NULL;
	char	   *pgtty = NULL;
	char	   *pgport = NULL;
	char	   *pgoptions = NULL;
	char	   *dbName;
	int			i;
	PGconn	   *conn;

	if (argc == 1)
	{
		Tcl_AppendResult(interp, "pg_connect: database name missing\n", 0);
		Tcl_AppendResult(interp, "pg_connect databaseName [-host hostName] [-port portNumber] [-tty pgtty]]\n", 0);
		Tcl_AppendResult(interp, "pg_connect -conninfo <conninfo-string>", 0);
		return TCL_ERROR;

	}

	if (!strcmp("-conninfo", argv[1]))
	{

		/*
		 * Establish a connection using the new PQconnectdb() interface
		 */
		if (argc != 3)
		{
			Tcl_AppendResult(interp, "pg_connect: syntax error\n", 0);
			Tcl_AppendResult(interp, "pg_connect -conninfo <conninfo-string>", 0);
			return TCL_ERROR;
		}
		conn = PQconnectdb(argv[2]);
	}
	else
	{

		/*
		 * Establish a connection using the old PQsetdb() interface
		 */
		if (argc > 2)
		{
			/* parse for pg environment settings */
			i = 2;
			while (i + 1 < argc)
			{
				if (strcmp(argv[i], "-host") == 0)
				{
					pghost = argv[i + 1];
					i += 2;
				}
				else if (strcmp(argv[i], "-port") == 0)
				{
					pgport = argv[i + 1];
					i += 2;
				}
				else if (strcmp(argv[i], "-tty") == 0)
				{
					pgtty = argv[i + 1];
					i += 2;
				}
				else if (strcmp(argv[i], "-options") == 0)
				{
					pgoptions = argv[i + 1];
					i += 2;
				}
				else
				{
					Tcl_AppendResult(interp, "Bad option to pg_connect : \n",
									 argv[i], 0);
					Tcl_AppendResult(interp, "pg_connect databaseName [-host hostName] [-port portNumber] [-tty pgtty]]", 0);
					return TCL_ERROR;
				}
			}					/* while */
			if ((i % 2 != 0) || i != argc)
			{
				Tcl_AppendResult(interp, "wrong # of arguments to pg_connect\n", argv[i], 0);
				Tcl_AppendResult(interp, "pg_connect databaseName [-host hostName] [-port portNumber] [-tty pgtty]]", 0);
				return TCL_ERROR;
			}
		}
		dbName = argv[1];
		conn = PQsetdb(pghost, pgport, pgoptions, pgtty, dbName);
	}

    if (PQstatus(conn) == CONNECTION_OK)
	{
		PgSetConnectionId(interp, conn);
		return TCL_OK;
	}
	else
	{
		Tcl_AppendResult(interp, "Connection to database failed\n",
			PQerrorMessage(conn), 0);
		PQfinish(conn);
		return TCL_ERROR;
	}
}


/**********************************
 * pg_disconnect
 close a backend connection

 syntax:
 pg_disconnect connection

 The argument passed in must be a connection pointer.

 **********************************/

int
Pg_disconnect(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	Tcl_Channel conn_chan;

	if (argc != 2)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n", "pg_disconnect connection", 0);
		return TCL_ERROR;
	}

	conn_chan = Tcl_GetChannel(interp, argv[1], 0);
	if (conn_chan == NULL)
	{
		Tcl_ResetResult(interp);
		Tcl_AppendResult(interp, argv[1], " is not a valid connection\n", 0);
		return TCL_ERROR;
	}

	return Tcl_UnregisterChannel(interp, conn_chan);
}

/**********************************
 * pg_exec
 send a query string to the backend connection

 syntax:
 pg_exec connection query

 the return result is either an error message or a handle for a query
 result.  Handles start with the prefix "pgp"
 **********************************/

int
Pg_exec(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	Pg_ConnectionId *connid;
	PGconn	   *conn;
	PGresult   *result;

	if (argc != 3)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_exec connection queryString", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], &connid);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	if (connid->res_copyStatus != RES_COPY_NONE)
	{
		Tcl_SetResult(interp, "Attempt to query while COPY in progress", TCL_STATIC);
		return TCL_ERROR;
	}

	result = PQexec(conn, argv[2]);

	/* Transfer any notify events from libpq to Tcl event queue. */
	PgNotifyTransferEvents(connid);

	if (result)
	{
		int			rId = PgSetResultId(interp, argv[1], result);

		ExecStatusType rStat = PQresultStatus(result);
		if (rStat == PGRES_COPY_IN || rStat == PGRES_COPY_OUT)
		{
			connid->res_copyStatus = RES_COPY_INPROGRESS;
			connid->res_copy = rId;
		}
		return TCL_OK;
	}
	else
	{
		/* error occurred during the query */
		Tcl_SetResult(interp, PQerrorMessage(conn), TCL_VOLATILE);
		return TCL_ERROR;
	}
}

/**********************************
 * pg_result
 get information about the results of a query

 syntax:
 pg_result result ?option?

 the options are:
 -status
 the status of the result
 -error
 the error message, if the status indicates error; otherwise an empty string
 -conn
 the connection that produced the result
 -oid
 if command was an INSERT, the OID of the inserted tuple
 -numTuples
 the number of tuples in the query
 -numAttrs
 returns the number of attributes returned by the query
 -assign arrayName
 assign the results to an array, using subscripts of the form
 (tupno,attributeName)
 -assignbyidx arrayName ?appendstr?
 assign the results to an array using the first field's value as a key.
 All but the first field of each tuple are stored, using subscripts of the form
 (field0value,attributeNameappendstr)
 -getTuple tupleNumber
 returns the values of the tuple in a list
 -tupleArray tupleNumber arrayName
 stores the values of the tuple in array arrayName, indexed
 by the attributes returned
 -attributes
 returns a list of the name/type pairs of the tuple attributes
 -lAttributes
 returns a list of the {name type len} entries of the tuple attributes
 -clear
 clear the result buffer. Do not reuse after this
 **********************************/
int
Pg_result(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGresult   *result;
	PGconn	   *conn;
	char	   *opt;
	int			i;
	int			tupno;
	char	   *arrVar;
	char		nameBuffer[256];
    const char *appendstr;

	if (argc < 3 || argc > 5)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n", 0);
		goto Pg_result_errReturn; /* append help info */
	}

	result = PgGetResultId(interp, argv[1]);
	if (result == (PGresult *) NULL)
	{
		Tcl_AppendResult(interp, argv[1], " is not a valid query result", 0);
		return TCL_ERROR;
	}

	opt = argv[2];

	if (strcmp(opt, "-status") == 0)
	{
		Tcl_AppendResult(interp, pgresStatus[PQresultStatus(result)], 0);
		return TCL_OK;
	}
	else if (strcmp(opt, "-error") == 0)
	{
		switch (PQresultStatus(result)) {
			case PGRES_EMPTY_QUERY:
			case PGRES_COMMAND_OK:
			case PGRES_TUPLES_OK:
			case PGRES_COPY_OUT:
			case PGRES_COPY_IN:
				Tcl_ResetResult(interp);
				break;
			default:
				if (PgGetConnByResultId(interp, argv[1]) != TCL_OK)
					return TCL_ERROR;
				conn = PgGetConnectionId(interp, interp->result,
										 (Pg_ConnectionId **) NULL);
				if (conn == (PGconn *) NULL)
					return TCL_ERROR;
				Tcl_SetResult(interp, PQerrorMessage(conn), TCL_VOLATILE);
				break;
		}
		return TCL_OK;
	}
	else if (strcmp(opt, "-conn") == 0)
		return PgGetConnByResultId(interp, argv[1]);
	else if (strcmp(opt, "-oid") == 0)
	{
		Tcl_AppendResult(interp, PQoidStatus(result), 0);
		return TCL_OK;
	}
	else if (strcmp(opt, "-clear") == 0)
	{
		PgDelResultId(interp, argv[1]);
		PQclear(result);
		return TCL_OK;
	}
	else if (strcmp(opt, "-numTuples") == 0)
	{
		sprintf(interp->result, "%d", PQntuples(result));
		return TCL_OK;
	}
	else if (strcmp(opt, "-numAttrs") == 0)
	{
		sprintf(interp->result, "%d", PQnfields(result));
		return TCL_OK;
	}
	else if (strcmp(opt, "-assign") == 0)
	{
		if (argc != 4)
		{
			Tcl_AppendResult(interp, "-assign option must be followed by a variable name", 0);
			return TCL_ERROR;
		}
		arrVar = argv[3];

		/*
		 * this assignment assigns the table of result tuples into a giant
		 * array with the name given in the argument.
		 * The indices of the array are of the form (tupno,attrName).
		 * Note we expect field names not to
		 * exceed a few dozen characters, so truncating to prevent buffer
		 * overflow shouldn't be a problem.
		 */
		for (tupno = 0; tupno < PQntuples(result); tupno++)
		{
			for (i = 0; i < PQnfields(result); i++)
			{
				sprintf(nameBuffer, "%d,%.200s", tupno, PQfname(result, i));
				if (Tcl_SetVar2(interp, arrVar, nameBuffer,
#ifdef TCL_ARRAYS
								tcl_value(PQgetvalue(result, tupno, i)),
#else
								PQgetvalue(result, tupno, i),
#endif
								TCL_LEAVE_ERR_MSG) == NULL)
					return TCL_ERROR;
			}
		}
		Tcl_AppendResult(interp, arrVar, 0);
		return TCL_OK;
	}
	else if (strcmp(opt, "-assignbyidx") == 0)
	{
		if (argc != 4 && argc != 5)
		{
			Tcl_AppendResult(interp, "-assignbyidx option requires an array name and optionally an append string",0);
			return TCL_ERROR;
		}
		arrVar = argv[3];
		appendstr = (argc == 5) ? (const char *) argv[4] : "";

		/*
		 * this assignment assigns the table of result tuples into a giant
		 * array with the name given in the argument.  The indices of the array
		 * are of the form (field0Value,attrNameappendstr).
		 * Here, we still assume PQfname won't exceed 200 characters,
		 * but we dare not make the same assumption about the data in field 0
		 * nor the append string.
		 */
		for (tupno = 0; tupno < PQntuples(result); tupno++)
		{
			const char *field0 = PQgetvalue(result, tupno, 0);
			char * workspace = malloc(strlen(field0) + strlen(appendstr) + 210);

			for (i = 1; i < PQnfields(result); i++)
			{
				sprintf(workspace, "%s,%.200s%s", field0, PQfname(result,i),
						appendstr);
				if (Tcl_SetVar2(interp, arrVar, workspace,
								PQgetvalue(result, tupno, i),
								TCL_LEAVE_ERR_MSG) == NULL)
				{
					free(workspace);
					return TCL_ERROR;
				}
			}
			free(workspace);
		}
		Tcl_AppendResult(interp, arrVar, 0);
		return TCL_OK;
	}
	else if (strcmp(opt, "-getTuple") == 0)
	{
		if (argc != 4)
		{
			Tcl_AppendResult(interp, "-getTuple option must be followed by a tuple number", 0);
			return TCL_ERROR;
		}
		tupno = atoi(argv[3]);
		if (tupno < 0 || tupno >= PQntuples(result))
		{
			Tcl_AppendResult(interp, "argument to getTuple cannot exceed number of tuples - 1", 0);
			return TCL_ERROR;
		}
		for (i = 0; i < PQnfields(result); i++)
			Tcl_AppendElement(interp, PQgetvalue(result, tupno, i));
		return TCL_OK;
	}
	else if (strcmp(opt, "-tupleArray") == 0)
	{
		if (argc != 5)
		{
			Tcl_AppendResult(interp, "-tupleArray option must be followed by a tuple number and array name", 0);
			return TCL_ERROR;
		}
		tupno = atoi(argv[3]);
		if (tupno < 0 || tupno >= PQntuples(result))
		{
			Tcl_AppendResult(interp, "argument to tupleArray cannot exceed number of tuples - 1", 0);
			return TCL_ERROR;
		}
		for (i = 0; i < PQnfields(result); i++)
		{
			if (Tcl_SetVar2(interp, argv[4], PQfname(result, i),
							PQgetvalue(result, tupno, i),
							TCL_LEAVE_ERR_MSG) == NULL)
				return TCL_ERROR;
		}
		return TCL_OK;
	}
	else if (strcmp(opt, "-attributes") == 0)
	{
		for (i = 0; i < PQnfields(result); i++)
			Tcl_AppendElement(interp, PQfname(result, i));
		return TCL_OK;
	}
	else if (strcmp(opt, "-lAttributes") == 0)
	{
		for (i = 0; i < PQnfields(result); i++)
		{
			/* start a sublist */
			if (i > 0)
				Tcl_AppendResult(interp, " {", 0);
			else
				Tcl_AppendResult(interp, "{", 0);
			Tcl_AppendElement(interp, PQfname(result, i));
			sprintf(nameBuffer, "%ld", (long) PQftype(result, i));
			Tcl_AppendElement(interp, nameBuffer);
			sprintf(nameBuffer, "%ld", (long) PQfsize(result, i));
			Tcl_AppendElement(interp, nameBuffer);
			/* end the sublist */
			Tcl_AppendResult(interp, "}", 0);
		}
		return TCL_OK;
	}
	else
	{
		Tcl_AppendResult(interp, "Invalid option\n", 0);
		goto Pg_result_errReturn; /* append help info */
	}


Pg_result_errReturn:
	Tcl_AppendResult(interp,
					 "pg_result result ?option? where option is\n",
					 "\t-status\n",
					 "\t-error\n",
					 "\t-conn\n",
					 "\t-oid\n",
					 "\t-numTuples\n",
					 "\t-numAttrs\n"
					 "\t-assign arrayVarName\n",
					 "\t-assignbyidx arrayVarName ?appendstr?\n",
					 "\t-getTuple tupleNumber\n",
					 "\t-tupleArray tupleNumber arrayVarName\n",
					 "\t-attributes\n"
					 "\t-lAttributes\n"
					 "\t-clear\n",
					 (char *) 0);
	return TCL_ERROR;


}

/**********************************
 * pg_lo_open
	 open a large object

 syntax:
 pg_lo_open conn objOid mode

 where mode can be either 'r', 'w', or 'rw'
**********************/

int
Pg_lo_open(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGconn	   *conn;
	int			lobjId;
	int			mode;
	int			fd;

	if (argc != 4)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_lo_open connection lobjOid mode", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], (Pg_ConnectionId **) NULL);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	lobjId = atoi(argv[2]);
	if (strlen(argv[3]) < 1 ||
		strlen(argv[3]) > 2)
	{
		Tcl_AppendResult(interp, "mode argument must be 'r', 'w', or 'rw'", 0);
		return TCL_ERROR;
	}
	switch (argv[3][0])
	{
		case 'r':
		case 'R':
			mode = INV_READ;
			break;
		case 'w':
		case 'W':
			mode = INV_WRITE;
			break;
		default:
			Tcl_AppendResult(interp, "mode argument must be 'r', 'w', or 'rw'", 0);
			return TCL_ERROR;
	}
	switch (argv[3][1])
	{
		case '\0':
			break;
		case 'r':
		case 'R':
			mode = mode & INV_READ;
			break;
		case 'w':
		case 'W':
			mode = mode & INV_WRITE;
			break;
		default:
			Tcl_AppendResult(interp, "mode argument must be 'r', 'w', or 'rw'", 0);
			return TCL_ERROR;
	}

	fd = lo_open(conn, lobjId, mode);
	sprintf(interp->result, "%d", fd);
	return TCL_OK;
}

/**********************************
 * pg_lo_close
	 close a large object

 syntax:
 pg_lo_close conn fd

**********************/
int
Pg_lo_close(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGconn	   *conn;
	int			fd;

	if (argc != 3)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_lo_close connection fd", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], (Pg_ConnectionId **) NULL);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	fd = atoi(argv[2]);
	sprintf(interp->result, "%d", lo_close(conn, fd));
	return TCL_OK;
}

/**********************************
 * pg_lo_read
	 reads at most len bytes from a large object into a variable named
 bufVar

 syntax:
 pg_lo_read conn fd bufVar len

 bufVar is the name of a variable in which to store the contents of the read

**********************/
int
Pg_lo_read(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGconn	   *conn;
	int			fd;
	int			nbytes = 0;
	char	   *buf;
	char	   *bufVar;
	int			len;

	if (argc != 5)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 " pg_lo_read conn fd bufVar len", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], (Pg_ConnectionId **) NULL);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	fd = atoi(argv[2]);

	bufVar = argv[3];

	len = atoi(argv[4]);

	if (len <= 0)
	{
		sprintf(interp->result, "%d", nbytes);
		return TCL_OK;
	}
	buf = ckalloc(len + 1);

	nbytes = lo_read(conn, fd, buf, len);

	Tcl_SetVar(interp, bufVar, buf, TCL_LEAVE_ERR_MSG);
	sprintf(interp->result, "%d", nbytes);
	ckfree(buf);
	return TCL_OK;

}

/***********************************
Pg_lo_write
   write at most len bytes to a large object

 syntax:
 pg_lo_write conn fd buf len

***********************************/
int
Pg_lo_write(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGconn	   *conn;
	char	   *buf;
	int			fd;
	int			nbytes = 0;
	int			len;

	if (argc != 5)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_lo_write conn fd buf len", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], (Pg_ConnectionId **) NULL);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	fd = atoi(argv[2]);

	buf = argv[3];

	len = atoi(argv[4]);

	if (len <= 0)
	{
		sprintf(interp->result, "%d", nbytes);
		return TCL_OK;
	}

	nbytes = lo_write(conn, fd, buf, len);
	sprintf(interp->result, "%d", nbytes);
	return TCL_OK;
}

/***********************************
Pg_lo_lseek
	seek to a certain position in a large object

syntax
  pg_lo_lseek conn fd offset whence

whence can be either
"SEEK_CUR", "SEEK_END", or "SEEK_SET"
***********************************/
int
Pg_lo_lseek(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGconn	   *conn;
	int			fd;
	char	   *whenceStr;
	int			offset,
				whence;

	if (argc != 5)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_lo_lseek conn fd offset whence", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], (Pg_ConnectionId **) NULL);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	fd = atoi(argv[2]);

	offset = atoi(argv[3]);

	whenceStr = argv[4];
	if (strcmp(whenceStr, "SEEK_SET") == 0)
		whence = SEEK_SET;
	else if (strcmp(whenceStr, "SEEK_CUR") == 0)
		whence = SEEK_CUR;
	else if (strcmp(whenceStr, "SEEK_END") == 0)
		whence = SEEK_END;
	else
	{
		Tcl_AppendResult(interp, "the whence argument to Pg_lo_lseek must be SEEK_SET, SEEK_CUR or SEEK_END", 0);
		return TCL_ERROR;
	}

	sprintf(interp->result, "%d", lo_lseek(conn, fd, offset, whence));
	return TCL_OK;
}


/***********************************
Pg_lo_creat
   create a new large object with mode

 syntax:
   pg_lo_creat conn mode

mode can be any OR'ing together of INV_READ, INV_WRITE,
for now, we don't support any additional storage managers.

***********************************/
int
Pg_lo_creat(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGconn	   *conn;
	char	   *modeStr;
	char	   *modeWord;
	int			mode;

	if (argc != 3)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_lo_creat conn mode", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], (Pg_ConnectionId **) NULL);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	modeStr = argv[2];

	modeWord = strtok(modeStr, "|");
	if (strcmp(modeWord, "INV_READ") == 0)
		mode = INV_READ;
	else if (strcmp(modeWord, "INV_WRITE") == 0)
		mode = INV_WRITE;
	else
	{
		Tcl_AppendResult(interp,
						 "invalid mode argument to Pg_lo_creat\nmode argument must be some OR'd combination of INV_READ, and INV_WRITE",
						 0);
		return TCL_ERROR;
	}

	while ((modeWord = strtok((char *) NULL, "|")) != NULL)
	{
		if (strcmp(modeWord, "INV_READ") == 0)
			mode |= INV_READ;
		else if (strcmp(modeWord, "INV_WRITE") == 0)
			mode |= INV_WRITE;
		else
		{
			Tcl_AppendResult(interp,
							 "invalid mode argument to Pg_lo_creat\nmode argument must be some OR'd combination of INV_READ, INV_WRITE",
							 0);
			return TCL_ERROR;
		}
	}
	sprintf(interp->result, "%d", lo_creat(conn, mode));
	return TCL_OK;
}

/***********************************
Pg_lo_tell
	returns the current seek location of the large object

 syntax:
   pg_lo_tell conn fd

***********************************/
int
Pg_lo_tell(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGconn	   *conn;
	int			fd;

	if (argc != 3)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_lo_tell conn fd", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], (Pg_ConnectionId **) NULL);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	fd = atoi(argv[2]);

	sprintf(interp->result, "%d", lo_tell(conn, fd));
	return TCL_OK;

}

/***********************************
Pg_lo_unlink
	unlink a file based on lobject id

 syntax:
   pg_lo_unlink conn lobjId


***********************************/
int
Pg_lo_unlink(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGconn	   *conn;
	int			lobjId;
	int			retval;

	if (argc != 3)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_lo_tell conn fd", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], (Pg_ConnectionId **) NULL);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	lobjId = atoi(argv[2]);

	retval = lo_unlink(conn, lobjId);
	if (retval == -1)
	{
		sprintf(interp->result, "Pg_lo_unlink of '%d' failed", lobjId);
		return TCL_ERROR;
	}

	sprintf(interp->result, "%d", retval);
	return TCL_OK;
}

/***********************************
Pg_lo_import
	import a Unix file into an (inversion) large objct
 returns the oid of that object upon success
 returns InvalidOid upon failure

 syntax:
   pg_lo_import conn filename

***********************************/

int
Pg_lo_import(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGconn	   *conn;
	char	   *filename;
	Oid			lobjId;

	if (argc != 3)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_lo_import conn filename", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], (Pg_ConnectionId **) NULL);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	filename = argv[2];

	lobjId = lo_import(conn, filename);
	if (lobjId == InvalidOid)
	{
		sprintf(interp->result, "Pg_lo_import of '%s' failed", filename);
		return TCL_ERROR;
	}
	sprintf(interp->result, "%d", lobjId);
	return TCL_OK;
}

/***********************************
Pg_lo_export
	export an Inversion large object to a Unix file

 syntax:
   pg_lo_export conn lobjId filename

***********************************/

int
Pg_lo_export(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	PGconn	   *conn;
	char	   *filename;
	Oid			lobjId;
	int			retval;

	if (argc != 4)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_lo_export conn lobjId filename", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], (Pg_ConnectionId **) NULL);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	lobjId = atoi(argv[2]);
	filename = argv[3];

	retval = lo_export(conn, lobjId, filename);
	if (retval == -1)
	{
		sprintf(interp->result, "Pg_lo_export %d %s failed", lobjId, filename);
		return TCL_ERROR;
	}
	return TCL_OK;
}

/**********************************
 * pg_select
 send a select query string to the backend connection

 syntax:
 pg_select connection query var proc

 The query must be a select statement
 The var is used in the proc as an array
 The proc is run once for each row found

 Originally I was also going to update changes but that has turned out
 to be not so simple.  Instead, the caller should get the OID of any
 table they want to update and update it themself in the loop.	I may
 try to write a simplified table lookup and update function to make
 that task a little easier.

 The return is either TCL_OK, TCL_ERROR or TCL_RETURN and interp->result
 may contain more information.
 **********************************/

int
Pg_select(ClientData cData, Tcl_Interp * interp, int argc, char **argv)
{
	Pg_ConnectionId *connid;
	PGconn	   *conn;
	PGresult   *result;
	int			r;
	size_t		tupno,
				column,
				ncols;
	Tcl_DString headers;
	char		buffer[2048];
	struct info_s
	{
		char	   *cname;
		int			change;
	}		   *info;

	if (argc != 5)
	{
		Tcl_AppendResult(interp, "Wrong # of arguments\n",
						 "pg_select connection queryString var proc", 0);
		return TCL_ERROR;
	}

	conn = PgGetConnectionId(interp, argv[1], &connid);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	if ((result = PQexec(conn, argv[2])) == 0)
	{
		/* error occurred during the query */
		Tcl_SetResult(interp, PQerrorMessage(conn), TCL_VOLATILE);
		return TCL_ERROR;
	}

	/* Transfer any notify events from libpq to Tcl event queue. */
	PgNotifyTransferEvents(connid);

	if ((info = (struct info_s *) ckalloc(sizeof(*info) * (ncols = PQnfields(result)))) == NULL)
	{
		Tcl_AppendResult(interp, "Not enough memory", 0);
		return TCL_ERROR;
	}

	Tcl_DStringInit(&headers);

	for (column = 0; column < ncols; column++)
	{
		info[column].cname = PQfname(result, column);
		info[column].change = 0;
		Tcl_DStringAppendElement(&headers, info[column].cname);
	}

	Tcl_SetVar2(interp, argv[3], ".headers", Tcl_DStringValue(&headers), 0);
	Tcl_DStringFree(&headers);
	sprintf(buffer, "%d", ncols);
	Tcl_SetVar2(interp, argv[3], ".numcols", buffer, 0);

	for (tupno = 0; tupno < PQntuples(result); tupno++)
	{
		sprintf(buffer, "%d", tupno);
		Tcl_SetVar2(interp, argv[3], ".tupno", buffer, 0);

		for (column = 0; column < ncols; column++)
			Tcl_SetVar2(interp, argv[3], info[column].cname, PQgetvalue(result, tupno, column), 0);

		Tcl_SetVar2(interp, argv[3], ".command", "update", 0);

		if ((r = Tcl_Eval(interp, argv[4])) != TCL_OK && r != TCL_CONTINUE)
		{
			if (r == TCL_BREAK)
				return TCL_OK;

			if (r == TCL_ERROR)
			{
				char		msg[60];

				sprintf(msg, "\n    (\"pg_select\" body line %d)",
						interp->errorLine);
				Tcl_AddErrorInfo(interp, msg);
			}

			return r;
		}
	}

	ckfree((void *) info);
	Tcl_UnsetVar(interp, argv[3], 0);
	Tcl_AppendResult(interp, "", 0);
	return TCL_OK;
}

/*
 * Test whether any callbacks are registered on this connection for
 * the given relation name.  NB: supplied name must be case-folded already.
 */

static int
Pg_have_listener (Pg_ConnectionId *connid, const char * relname)
{
	Pg_TclNotifies *notifies;
	Tcl_HashEntry *entry;

	for (notifies = connid->notify_list;
		 notifies != NULL;
		 notifies = notifies->next)
	{
		Tcl_Interp *interp = notifies->interp;

		if (interp == NULL)
			continue;			/* ignore deleted interpreter */

		entry = Tcl_FindHashEntry(&notifies->notify_hash, relname);
		if (entry == NULL)
			continue;			/* no pg_listen in this interpreter */

		return TRUE;			/* OK, there is a listener */
	}

	return FALSE;				/* Found no listener */
}

/***********************************
Pg_listen
	create or remove a callback request for notifies on a given name

 syntax:
   pg_listen conn notifyname ?callbackcommand?

   With a fourth arg, creates or changes the callback command for
   notifies on the given name; without, cancels the callback request.

   Callbacks can occur whenever Tcl is executing its event loop.
   This is the normal idle loop in Tk; in plain tclsh applications,
   vwait or update can be used to enter the Tcl event loop.
***********************************/
int
Pg_listen(ClientData cData, Tcl_Interp * interp, int argc, char *argv[])
{
	char	   *origrelname;
	char	   *caserelname;
	char	   *callback = NULL;
	Pg_TclNotifies *notifies;
	Tcl_HashEntry *entry;
	Pg_ConnectionId *connid;
	PGconn	   *conn;
	PGresult   *result;
	int			new;

	if (argc < 3 || argc > 4)
	{
		Tcl_AppendResult(interp, "wrong # args, should be \"",
						 argv[0], " connection relname ?callback?\"", 0);
		return TCL_ERROR;
	}

	/*
	 * Get the command arguments. Note that the relation name will be
	 * copied by Tcl_CreateHashEntry while the callback string must be
	 * allocated by us.
	 */
	conn = PgGetConnectionId(interp, argv[1], &connid);
	if (conn == (PGconn *) NULL)
		return TCL_ERROR;

	/*
	 * LISTEN/NOTIFY do not preserve case unless the relation name is
	 * quoted.	We have to do the same thing to ensure that we will find
	 * the desired pg_listen item.
	 */
	origrelname = argv[2];
	caserelname = (char *) ckalloc((unsigned) (strlen(origrelname) + 1));
	if (*origrelname == '"')
	{
		/* Copy a quoted string without downcasing */
		strcpy(caserelname, origrelname + 1);
		caserelname[strlen(caserelname) - 1] = '\0';
	}
	else
	{
		/* Downcase it */
		char	   *rels = origrelname;
		char	   *reld = caserelname;

		while (*rels)
			*reld++ = tolower(*rels++);
		*reld = '\0';
	}

	if ((argc > 3) && *argv[3])
	{
		callback = (char *) ckalloc((unsigned) (strlen(argv[3]) + 1));
		strcpy(callback, argv[3]);
	}

	/* Find or make a Pg_TclNotifies struct for this interp and connection */

	for (notifies = connid->notify_list; notifies; notifies = notifies->next)
	{
		if (notifies->interp == interp)
			break;
	}
	if (notifies == NULL)
	{
		notifies = (Pg_TclNotifies *) ckalloc(sizeof(Pg_TclNotifies));
		notifies->interp = interp;
		Tcl_InitHashTable(&notifies->notify_hash, TCL_STRING_KEYS);
		notifies->next = connid->notify_list;
		connid->notify_list = notifies;
		Tcl_CallWhenDeleted(interp, PgNotifyInterpDelete,
							(ClientData) notifies);
	}

	if (callback)
	{
		/*
		 * Create or update a callback for a relation
		 */
		int alreadyHadListener = Pg_have_listener(connid, caserelname);

		entry = Tcl_CreateHashEntry(&notifies->notify_hash, caserelname, &new);
		/* If update, free the old callback string */
		if (! new)
			ckfree((char *) Tcl_GetHashValue(entry));
		/* Store the new callback string */
		Tcl_SetHashValue(entry, callback);

		/* Start the notify event source if it isn't already running */
		PgStartNotifyEventSource(connid);

		/*
		 * Send a LISTEN command if this is the first listener.
		 */
		if (! alreadyHadListener)
		{
			char   *cmd = (char *)
				ckalloc((unsigned) (strlen(origrelname) + 8));
			sprintf(cmd, "LISTEN %s", origrelname);
			result = PQexec(conn, cmd);
			ckfree(cmd);
			/* Transfer any notify events from libpq to Tcl event queue. */
			PgNotifyTransferEvents(connid);
			if (PQresultStatus(result) != PGRES_COMMAND_OK)
			{
				/* Error occurred during the execution of command */
				PQclear(result);
				Tcl_DeleteHashEntry(entry);
				ckfree(callback);
				ckfree(caserelname);
				Tcl_SetResult(interp, PQerrorMessage(conn), TCL_VOLATILE);
				return TCL_ERROR;
			}
			PQclear(result);
		}
	}
	else
	{
		/*
		 * Remove a callback for a relation
		 */
		entry = Tcl_FindHashEntry(&notifies->notify_hash, caserelname);
		if (entry == NULL)
		{
			Tcl_AppendResult(interp, "not listening on ", origrelname, 0);
			ckfree(caserelname);
			return TCL_ERROR;
		}
		ckfree((char *) Tcl_GetHashValue(entry));
		Tcl_DeleteHashEntry(entry);
		/*
		 * Send an UNLISTEN command if that was the last listener.
		 * Note: we don't attempt to turn off the notify mechanism
		 * if no LISTENs remain active; not worth the trouble.
		 */
		if (! Pg_have_listener(connid, caserelname))
		{
			char   *cmd = (char *)
				ckalloc((unsigned) (strlen(origrelname) + 10));
			sprintf(cmd, "UNLISTEN %s", origrelname);
			result = PQexec(conn, cmd);
			ckfree(cmd);
			/* Transfer any notify events from libpq to Tcl event queue. */
			PgNotifyTransferEvents(connid);
			if (PQresultStatus(result) != PGRES_COMMAND_OK)
			{
				/* Error occurred during the execution of command */
				PQclear(result);
				ckfree(caserelname);
				Tcl_SetResult(interp, PQerrorMessage(conn), TCL_VOLATILE);
				return TCL_ERROR;
			}
			PQclear(result);
		}
	}

	ckfree(caserelname);
	return TCL_OK;
}
