/*
 * PyGres, version 2.2 A Python interface for PostgreSQL database. Written by
 * D'Arcy J.M. Cain, (darcy@druid.net).  Based heavily on code written by
 * Pascal Andre, andre@chimay.via.ecp.fr. Copyright (c) 1995, Pascal Andre
 * (andre@via.ecp.fr).
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written
 * agreement is hereby granted, provided that the above copyright notice and
 * this paragraph and the following two paragraphs appear in all copies or in
 * any new file that contains a substantial portion of this file.
 *
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT,
 * SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS,
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE
 * AUTHOR HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND THE
 * AUTHOR HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES,
 * ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Further modifications copyright 1997, 1998, 1999 by D'Arcy J.M. Cain
 * (darcy@druid.net) subject to the same terms and conditions as above.
 *
 */

#include <Python.h>
#include <postgres.h>
#include <libpq-fe.h>
#include <libpq/libpq-fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* really bad stuff here - I'm so naughty */
/* If you need to you can run mkdefines to get */
/* current defines but it should not have changed */
#define INT2OID		21
#define INT4OID		23
#define OIDOID		26
#define FLOAT4OID	700
#define FLOAT8OID	701
#define CASHOID		790

static PyObject *PGError;
static const char *PyPgVersion = "3.1";

/* taken from fileobject.c */
#define BUF(v) PyString_AS_STRING((PyStringObject *)(v))

/* default values */
#define MODULE_NAME			"pgsql"
#define ARRAYSIZE			1

/* flags for object validity checks */
#define CHECK_OPEN			1
#define CHECK_CLOSE			2
#define CHECK_CNX			4
#define CHECK_RESULT		8
#define CHECK_DQL			16

/* query result types */
#define RESULT_EMPTY		1
#define RESULT_DML			2
#define RESULT_DDL			3
#define RESULT_DQL			4

/* flags for move methods */
#define QUERY_MOVEFIRST		1
#define QUERY_MOVELAST		2
#define QUERY_MOVENEXT		3
#define QUERY_MOVEPREV		4

/* moves names for errors */
const char *__movename[5] =
{"", "movefirst", "movelast", "movenext", "moveprev"};

#define MAX_BUFFER_SIZE 8192	/* maximum transaction size */

#ifndef NO_DIRECT
#define DIRECT_ACCESS	1		/* enables direct access functions */
#endif	 /* NO_DIRECT */

#ifndef NO_LARGE
#define LARGE_OBJECTS	1		/* enables large objects support */
#endif	 /* NO_LARGE */

#ifndef NO_DEF_VAR
#define DEFAULT_VARS	1		/* enables default variables use */
#endif	 /* NO_DEF_VAR */

#ifdef MS_WIN32
#define NO_SNPRINTF 1
#endif

/* --------------------------------------------------------------------- */

/* MODULE GLOBAL VARIABLES */

#ifdef DEFAULT_VARS

static PyObject *pg_default_host;		/* default database host */
static PyObject *pg_default_base;		/* default database name */
static PyObject *pg_default_opt;/* default connection options */
static PyObject *pg_default_tty;/* default debug tty */
static PyObject *pg_default_port;		/* default connection port */
static PyObject *pg_default_user;		/* default username */
static PyObject *pg_default_passwd;		/* default password */

#endif	 /* DEFAULT_VARS */

/* --------------------------------------------------------------------- */
/* OBJECTS DECLARATION */

/* pg connection object */

typedef struct
{
	PyObject_HEAD
	int			valid;			/* validity flag */
	PGconn	   *cnx;			/* PostGres connection handle */
	PGresult   *last_result;	/* last result content */
}			pgobject;

staticforward PyTypeObject PgType;

#define is_pgobject(v) ((v)->ob_type == &PgType)

static PyObject *
pgobject_New(void)
{
	pgobject   *pgobj;

	if ((pgobj = PyObject_NEW(pgobject, &PgType)) == NULL)
		return NULL;

	pgobj->valid = 1;
	pgobj->last_result = NULL;
	pgobj->cnx = NULL;
	return (PyObject *) pgobj;
}

/* pg query object */

typedef struct
{
	PyObject_HEAD
	PGresult   *last_result;	/* last result content */
	int			result_type;	/* type of previous result */
	long		current_pos;	/* current position in last result */
	long		num_rows;		/* number of (affected) rows */
}			pgqueryobject;

staticforward PyTypeObject PgQueryType;

#define is_pgqueryobject(v) ((v)->ob_type == &PgQueryType)

/* pg source object */

typedef struct
{
	PyObject_HEAD
	int			valid;			/* validity flag */
	pgobject   *pgcnx;			/* parent connection object */
	PGresult   *last_result;	/* last result content */
	int			result_type;	/* result type (DDL/DML/DQL) */
	long		arraysize;		/* array size for fetch method */
	int			current_row;	/* current selected row */
	int			max_row;		/* number of rows in the result */
	int			num_fields;		/* number of fields in each row */
}			pgsourceobject;

staticforward PyTypeObject PgSourceType;

#define is_pgsourceobject(v) ((v)->ob_type == &PgSourceType)


#ifdef LARGE_OBJECTS
/* pg large object */

typedef struct
{
	PyObject_HEAD
	pgobject * pgcnx;			/* parent connection object */
	Oid			lo_oid;			/* large object oid */
	int			lo_fd;			/* large object fd */
}			pglargeobject;

staticforward PyTypeObject PglargeType;

#define is_pglargeobject(v) ((v)->ob_type == &PglargeType)
#endif	 /* LARGE_OBJECTS */

/* --------------------------------------------------------------------- */
/* INTERNAL FUNCTIONS */

/* checks connection validity */
static int
check_cnx_obj(pgobject * self)
{
	if (!self->valid)
	{
		PyErr_SetString(PGError, "connection has been closed");
		return 0;
	}
	return 1;
}

#ifdef LARGE_OBJECTS
/* checks large object validity */
static int
check_lo_obj(pglargeobject * self, int level)
{
	if (!check_cnx_obj(self->pgcnx))
		return 0;

	if (!self->lo_oid)
	{
		PyErr_SetString(PGError, "object is not valid (null oid).");
		return 0;
	}

	if (level & CHECK_OPEN)
	{
		if (self->lo_fd < 0)
		{
			PyErr_SetString(PyExc_IOError, "object is not opened.");
			return 0;
		}
	}

	if (level & CHECK_CLOSE)
	{
		if (self->lo_fd >= 0)
		{
			PyErr_SetString(PyExc_IOError, "object is already opened.");
			return 0;
		}
	}

	return 1;
}

#endif	 /* LARGE_OBJECTS */

/* checks source object validity */
static int
check_source_obj(pgsourceobject * self, int level)
{
	if (!self->valid)
	{
		PyErr_SetString(PGError, "object has been closed");
		return 0;
	}

	if ((level & CHECK_RESULT) && self->last_result == NULL)
	{
		PyErr_SetString(PGError, "no result.");
		return 0;
	}

	if ((level & CHECK_DQL) && self->result_type != RESULT_DQL)
	{
		PyErr_SetString(PGError, "last query did not return tuples.");
		return 0;
	}

	if ((level & CHECK_CNX) && !check_cnx_obj(self->pgcnx))
		return 0;

	return 1;
}

/* shared functions for converting PG types to Python types */
int *
get_type_array(PGresult *result, int nfields)
{
	int		   *typ;
	int			j;

	if ((typ = malloc(sizeof(int) * nfields)) == NULL)
	{
		PyErr_SetString(PyExc_SyntaxError, "memory error in getresult().");
		return NULL;
	}

	for (j = 0; j < nfields; j++)
	{
		switch (PQftype(result, j))
		{
			case INT2OID:
			case INT4OID:
			case OIDOID:
				typ[j] = 1;
				break;

			case FLOAT4OID:
			case FLOAT8OID:
				typ[j] = 2;
				break;

			case CASHOID:
				typ[j] = 3;
				break;

			default:
				typ[j] = 4;
				break;
		}
	}

	return typ;
}


/* prototypes for constructors */
static pgsourceobject *pgsource_new(pgobject * pgcnx);

/* --------------------------------------------------------------------- */
/* PG SOURCE OBJECT IMPLEMENTATION */

/* constructor (internal use only) */
static pgsourceobject *
pgsource_new(pgobject * pgcnx)
{
	pgsourceobject *npgobj;

	/* allocates new query object */
	if ((npgobj = PyObject_NEW(pgsourceobject, &PgSourceType)) == NULL)
		return NULL;

	/* initializes internal parameters */
	Py_XINCREF(pgcnx);
	npgobj->pgcnx = pgcnx;
	npgobj->last_result = NULL;
	npgobj->valid = 1;
	npgobj->arraysize = ARRAYSIZE;

	return npgobj;
}

/* destructor */
static void
pgsource_dealloc(pgsourceobject * self)
{
	if (self->last_result)
		PQclear(self->last_result);

	Py_XDECREF(self->pgcnx);
	PyMem_DEL(self);
}

/* closes object */
static char pgsource_close__doc__[] =
"close() -- close query object without deleting it. "
"All instances of the query object can no longer be used after this call.";

static PyObject *
pgsource_close(pgsourceobject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_TypeError, "method close() takes no parameter.");
		return NULL;
	}

	/* frees result if necessary and invalidates object */
	if (self->last_result)
	{
		PQclear(self->last_result);
		self->result_type = RESULT_EMPTY;
		self->last_result = NULL;
	}

	self->valid = 0;

	/* return None */
	Py_INCREF(Py_None);
	return Py_None;
}

/* database query */
static char pgsource_execute__doc__[] =
"execute(sql) -- execute a SQL statement (string).\n "
"On success, this call returns the number of affected rows, "
"or None for DQL (SELECT, ...) statements.\n"
"The fetch (fetch(), fetchone() and fetchall()) methods can be used "
"to get result rows.";

static PyObject *
pgsource_execute(pgsourceobject * self, PyObject * args)
{
	char	   *query;
	const char *temp;
	long		num_rows;

	/* checks validity */
	if (!check_source_obj(self, CHECK_CNX))
		return NULL;

	/* get query args */
	if (!PyArg_ParseTuple(args, "s", &query))
	{
		PyErr_SetString(PyExc_TypeError, "execute(sql), with sql (string).");
		return NULL;
	}

	/* frees previous result */
	if (self->last_result)
	{
		PQclear(self->last_result);
		self->last_result = NULL;
	}
	self->max_row = 0;
	self->current_row = 0;
	self->num_fields = 0;

	/* gets result */
	Py_BEGIN_ALLOW_THREADS
		self->last_result = PQexec(self->pgcnx->cnx, query);
	Py_END_ALLOW_THREADS

	/* checks result validity */
		if (!self->last_result)
	{
		PyErr_SetString(PyExc_ValueError, PQerrorMessage(self->pgcnx->cnx));
		return NULL;
	}

	/* checks result status */
	switch (PQresultStatus(self->last_result))
	{

			/* query succeeded */
		case PGRES_TUPLES_OK:	/* DQL: returns None (DB-SIG compliant) */
			self->result_type = RESULT_DQL;
			self->max_row = PQntuples(self->last_result);
			self->num_fields = PQnfields(self->last_result);
			Py_INCREF(Py_None);
			return Py_None;
		case PGRES_COMMAND_OK:	/* other requests */
		case PGRES_COPY_OUT:
		case PGRES_COPY_IN:
			self->result_type = RESULT_DDL;
			temp = PQcmdTuples(self->last_result);
			num_rows = 1;
			if (temp[0] != 0)
			{
				self->result_type = RESULT_DML;
				num_rows = atol(temp);
			}
			return PyInt_FromLong(num_rows);

			/* query failed */
		case PGRES_EMPTY_QUERY:
			PyErr_SetString(PyExc_ValueError, "empty query.");
			break;
		case PGRES_BAD_RESPONSE:
		case PGRES_FATAL_ERROR:
		case PGRES_NONFATAL_ERROR:
			PyErr_SetString(PGError, PQerrorMessage(self->pgcnx->cnx));
			break;
		default:
			PyErr_SetString(PGError, "internal error: "
							"unknown result status.");
			break;
	}

	/* frees result and returns error */
	PQclear(self->last_result);
	self->last_result = NULL;
	self->result_type = RESULT_EMPTY;
	return NULL;
}

/* gets oid status for last query (valid for INSERTs, 0 for other) */
static char pgsource_oidstatus__doc__[] =
"oidstatus() -- return oid of last inserted row (if available).";

static PyObject *
pgsource_oidstatus(pgsourceobject * self, PyObject * args)
{
	long		oid;
	const char *status;

	/* checks validity */
	if (!check_source_obj(self, CHECK_RESULT))
		return NULL;

	/* checks args */
	if ((args != NULL) && (!PyArg_ParseTuple(args, "")))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method oidstatus() takes no parameters.");
		return NULL;
	}

	/* retrieves oid status */
	if ((oid = PQoidValue(self->last_result)) == InvalidOid)
		oid = 0;

	return PyInt_FromLong(oid);
}

/* fetches rows from last result */
static char pgsource_fetch__doc__[] =
"fetch(num) -- return the next num rows from the last result in a list. "
"If num parameter is omitted arraysize attribute value is used. "
"If size equals -1, all rows are fetched.";

static PyObject *
pgsource_fetch(pgsourceobject * self, PyObject * args)
{
	PyObject   *rowtuple,
			   *reslist,
			   *str;
	int			i,
				j;
	long		size;

	/* checks validity */
	if (!check_source_obj(self, CHECK_RESULT | CHECK_DQL))
		return NULL;

	/* checks args */
	size = self->arraysize;
	if (!PyArg_ParseTuple(args, "|l", &size))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"fetch(num), with num (integer, optional).");
		return NULL;
	}

	/* seeks last line */
	/* limit size to be within the amount of data we actually have */
	if (size == -1 || (self->max_row - self->current_row) < size)
		size = self->max_row - self->current_row;

	/* allocate list for result */
	if ((reslist = PyList_New(0)) == NULL)
		return NULL;

	/* builds result */
	for (i = 0; i < size; ++i)
	{
		if ((rowtuple = PyTuple_New(self->num_fields)) == NULL)
		{
			Py_DECREF(reslist);
			return NULL;
		}

		for (j = 0; j < self->num_fields; j++)
		{
			if (PQgetisnull(self->last_result, i, j))
			{
				Py_INCREF(Py_None);
				str = Py_None;
			}
			else
				str = PyString_FromString(PQgetvalue(self->last_result, i, j));

			PyTuple_SET_ITEM(rowtuple, j, str);
		}

		PyList_Append(reslist, rowtuple);
		self->current_row++;
	}

	return reslist;
}

/* changes current row (internal wrapper for all "move" methods) */
static PyObject *
pgsource_move(pgsourceobject * self, PyObject * args, int move)
{
	/* checks validity */
	if (!check_source_obj(self, CHECK_RESULT | CHECK_DQL))
		return NULL;

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		char		errbuf[256];

#ifdef	NO_SNPRINTF
		sprintf(errbuf, "method %s() takes no parameter.", __movename[move]);
#else
		snprintf(errbuf, sizeof(errbuf),
				 "method %s() takes no parameter.", __movename[move]);
#endif

		PyErr_SetString(PyExc_SyntaxError, errbuf);
		return NULL;
	}

	/* changes the current row */
	switch (move)
	{
		case QUERY_MOVEFIRST:
			self->current_row = 0;
			break;
		case QUERY_MOVELAST:
			self->current_row = self->max_row - 1;
			break;
		case QUERY_MOVENEXT:
			if (self->current_row != self->max_row)
				self->current_row++;
			break;
		case QUERY_MOVEPREV:
			if (self->current_row > 0)
				self->current_row--;
			break;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

/* move to first result row */
static char pgsource_movefirst__doc__[] =
"movefirst() -- move to first result row.";

static PyObject *
pgsource_movefirst(pgsourceobject * self, PyObject * args)
{
	return pgsource_move(self, args, QUERY_MOVEFIRST);
}

/* move to last result row */
static char pgsource_movelast__doc__[] =
"movelast() -- move to last valid result row.";

static PyObject *
pgsource_movelast(pgsourceobject * self, PyObject * args)
{
	return pgsource_move(self, args, QUERY_MOVELAST);
}

/* move to next result row */
static char pgsource_movenext__doc__[] =
"movenext() -- move to next result row.";

static PyObject *
pgsource_movenext(pgsourceobject * self, PyObject * args)
{
	return pgsource_move(self, args, QUERY_MOVENEXT);
}

/* move to previous result row */
static char pgsource_moveprev__doc__[] =
"moveprev() -- move to previous result row.";

static PyObject *
pgsource_moveprev(pgsourceobject * self, PyObject * args)
{
	return pgsource_move(self, args, QUERY_MOVEPREV);
}

/* finds field number from string/integer (internal use only) */
static int
pgsource_fieldindex(pgsourceobject * self, PyObject * param, const char *usage)
{
	int			num;

	/* checks validity */
	if (!check_source_obj(self, CHECK_RESULT | CHECK_DQL))
		return -1;

	/* gets field number */
	if (PyString_Check(param))
		num = PQfnumber(self->last_result, PyString_AsString(param));
	else if (PyInt_Check(param))
		num = PyInt_AsLong(param);
	else
	{
		PyErr_SetString(PyExc_TypeError, usage);
		return -1;
	}

	/* checks field validity */
	if (num < 0 || num >= self->num_fields)
	{
		PyErr_SetString(PyExc_ValueError, "Unknown field.");
		return -1;
	}

	return num;
}

/* builds field information from position (internal use only) */
static PyObject *
pgsource_buildinfo(pgsourceobject * self, int num)
{
	PyObject   *result;

	/* allocates tuple */
	result = PyTuple_New(3);
	if (!result)
		return NULL;

	/* affects field information */
	PyTuple_SET_ITEM(result, 0, PyInt_FromLong(num));
	PyTuple_SET_ITEM(result, 1,
				   PyString_FromString(PQfname(self->last_result, num)));
	PyTuple_SET_ITEM(result, 2,
					 PyInt_FromLong(PQftype(self->last_result, num)));

	return result;
}

/* lists fields info */
static char pgsource_listinfo__doc__[] =
"listinfo() -- return information for all fields "
"(position, name, type oid).";

static PyObject *
pgsource_listinfo(pgsourceobject * self, PyObject * args)
{
	int			i;
	PyObject   *result,
			   *info;

	/* checks validity */
	if (!check_source_obj(self, CHECK_RESULT | CHECK_DQL))
		return NULL;

	/* gets args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_TypeError,
						"method listinfo() takes no parameter.");
		return NULL;
	}

	/* builds result */
	if ((result = PyTuple_New(self->num_fields)) == NULL)
		return NULL;

	for (i = 0; i < self->num_fields; i++)
	{
		info = pgsource_buildinfo(self, i);
		if (!info)
		{
			Py_DECREF(result);
			return NULL;
		}
		PyTuple_SET_ITEM(result, i, info);
	}

	/* returns result */
	return result;
};

/* list fields information for last result */
static char pgsource_fieldinfo__doc__[] =
"fieldinfo(string|integer) -- return specified field information "
"(position, name, type oid).";

static PyObject *
pgsource_fieldinfo(pgsourceobject * self, PyObject * args)
{
	static const char short_usage[] =
	"fieldinfo(desc), with desc (string|integer).";
	int			num;
	PyObject   *param;

	/* gets args */
	if (!PyArg_ParseTuple(args, "O", &param))
	{
		PyErr_SetString(PyExc_TypeError, short_usage);
		return NULL;
	}

	/* checks args and validity */
	if ((num = pgsource_fieldindex(self, param, short_usage)) == -1)
		return NULL;

	/* returns result */
	return pgsource_buildinfo(self, num);
};

/* retrieve field value */
static char pgsource_field__doc__[] =
"field(string|integer) -- return specified field value.";

static PyObject *
pgsource_field(pgsourceobject * self, PyObject * args)
{
	static const char short_usage[] =
	"field(desc), with desc (string|integer).";
	int			num;
	PyObject   *param;

	/* gets args */
	if (!PyArg_ParseTuple(args, "O", &param))
	{
		PyErr_SetString(PyExc_TypeError, short_usage);
		return NULL;
	}

	/* checks args and validity */
	if ((num = pgsource_fieldindex(self, param, short_usage)) == -1)
		return NULL;

	return PyString_FromString(PQgetvalue(self->last_result,
										  self->current_row, num));
}

/* query object methods */
static PyMethodDef pgsource_methods[] = {
	{"close", (PyCFunction) pgsource_close, 1,
	pgsource_close__doc__},
	{"execute", (PyCFunction) pgsource_execute, 1,
	pgsource_execute__doc__},
	{"oidstatus", (PyCFunction) pgsource_oidstatus, 1,
	pgsource_oidstatus__doc__},
	{"fetch", (PyCFunction) pgsource_fetch, 1,
	pgsource_fetch__doc__},
	{"movefirst", (PyCFunction) pgsource_movefirst, 1,
	pgsource_movefirst__doc__},
	{"movelast", (PyCFunction) pgsource_movelast, 1,
	pgsource_movelast__doc__},
	{"movenext", (PyCFunction) pgsource_movenext, 1,
	pgsource_movenext__doc__},
	{"moveprev", (PyCFunction) pgsource_moveprev, 1,
	pgsource_moveprev__doc__},
	{"field", (PyCFunction) pgsource_field, 1,
	pgsource_field__doc__},
	{"fieldinfo", (PyCFunction) pgsource_fieldinfo, 1,
	pgsource_fieldinfo__doc__},
	{"listinfo", (PyCFunction) pgsource_listinfo, 1,
	pgsource_listinfo__doc__},
	{NULL, NULL}
};

/* gets query object attributes */
static PyObject *
pgsource_getattr(pgsourceobject * self, char *name)
{
	/* pg connection object */
	if (!strcmp(name, "pgcnx"))
	{
		if (check_source_obj(self, 0))
		{
			Py_INCREF(self->pgcnx);
			return (PyObject *) (self->pgcnx);
		}
		Py_INCREF(Py_None);
		return Py_None;
	}

	/* arraysize */
	if (!strcmp(name, "arraysize"))
		return PyInt_FromLong(self->arraysize);

	/* resulttype */
	if (!strcmp(name, "resulttype"))
		return PyInt_FromLong(self->result_type);

	/* ntuples */
	if (!strcmp(name, "ntuples"))
		return PyInt_FromLong(self->max_row);

	/* nfields */
	if (!strcmp(name, "nfields"))
		return PyInt_FromLong(self->num_fields);

	/* attributes list */
	if (!strcmp(name, "__members__"))
	{
		PyObject   *list = PyList_New(5);

		PyList_SET_ITEM(list, 0, PyString_FromString("pgcnx"));
		PyList_SET_ITEM(list, 1, PyString_FromString("arraysize"));
		PyList_SET_ITEM(list, 2, PyString_FromString("resulttype"));
		PyList_SET_ITEM(list, 3, PyString_FromString("ntuples"));
		PyList_SET_ITEM(list, 4, PyString_FromString("nfields"));

		return list;
	}

	/* module name */
	if (!strcmp(name, "__module__"))
		return PyString_FromString(MODULE_NAME);

	/* class name */
	if (!strcmp(name, "__class__"))
		return PyString_FromString("pgsource");

	/* seeks name in methods (fallback) */
	return Py_FindMethod(pgsource_methods, (PyObject *) self, name);
}

/* sets query object attributes */
static int
pgsource_setattr(pgsourceobject * self, char *name, PyObject * v)
{
	/* arraysize */
	if (!strcmp(name, "arraysize"))
	{
		if (!PyInt_Check(v))
		{
			PyErr_SetString(PyExc_TypeError, "arraysize must be integer.");
			return -1;
		}

		self->arraysize = PyInt_AsLong(v);
		return 0;
	}

	/* unknown attribute */
	PyErr_SetString(PyExc_TypeError, "not a writable attribute.");
	return -1;
}

/* prints query object in human readable format */
static int
pgsource_print(pgsourceobject * self, FILE *fp, int flags)
{
	PQprintOpt	op;

	switch (self->result_type)
	{
		case RESULT_DQL:
			memset(&op, 0, sizeof(op));
			op.align = 1;
			op.header = 1;
			op.fieldSep = "|";
			op.pager = 1;
			PQprint(fp, self->last_result, &op);
			break;
		case RESULT_DDL:
		case RESULT_DML:
			fputs(PQcmdStatus(self->last_result), fp);
			break;
		case RESULT_EMPTY:
		default:
			fputs("Empty PostgreSQL source object.", fp);
			break;
	}

	return 0;
}

/* query type definition */
staticforward PyTypeObject PgSourceType = {
	PyObject_HEAD_INIT(NULL)

	0,							/* ob_size */
	"pgsourceobject",			/* tp_name */
	sizeof(pgsourceobject),		/* tp_basicsize */
	0,							/* tp_itemsize */
	/* methods */
	(destructor) pgsource_dealloc,		/* tp_dealloc */
	(printfunc) pgsource_print, /* tp_print */
	(getattrfunc) pgsource_getattr,		/* tp_getattr */
	(setattrfunc) pgsource_setattr,		/* tp_setattr */
	0,							/* tp_compare */
	0,							/* tp_repr */
	0,							/* tp_as_number */
	0,							/* tp_as_sequence */
	0,							/* tp_as_mapping */
	0,							/* tp_hash */
};

/* --------------------------------------------------------------------- */
/* PG "LARGE" OBJECT IMPLEMENTATION */

#ifdef LARGE_OBJECTS

/* constructor (internal use only) */
static pglargeobject *
pglarge_new(pgobject * pgcnx, Oid oid)
{
	pglargeobject *npglo;

	if ((npglo = PyObject_NEW(pglargeobject, &PglargeType)) == NULL)
		return NULL;

	Py_XINCREF(pgcnx);
	npglo->pgcnx = pgcnx;
	npglo->lo_fd = -1;
	npglo->lo_oid = oid;

	return npglo;
}

/* destructor */
static void
pglarge_dealloc(pglargeobject * self)
{
	if (self->lo_fd >= 0 && check_cnx_obj(self->pgcnx))
		lo_close(self->pgcnx->cnx, self->lo_fd);

	Py_XDECREF(self->pgcnx);
	PyMem_DEL(self);
}

/* opens large object */
static char pglarge_open__doc__[] =
"open(mode) -- open access to large object with specified mode "
"(INV_READ, INV_WRITE constants defined by module).";

static PyObject *
pglarge_open(pglargeobject * self, PyObject * args)
{
	int			mode,
				fd;

	/* check validity */
	if (!check_lo_obj(self, CHECK_CLOSE))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "i", &mode))
	{
		PyErr_SetString(PyExc_TypeError, "open(mode), with mode(integer).");
		return NULL;
	}

	/* opens large object */
	if ((fd = lo_open(self->pgcnx->cnx, self->lo_oid, mode)) < 0)
	{
		PyErr_SetString(PyExc_IOError, "can't open large object.");
		return NULL;
	}
	self->lo_fd = fd;

	/* no error : returns Py_None */
	Py_INCREF(Py_None);
	return Py_None;
}

/* close large object */
static char pglarge_close__doc__[] =
"close() -- close access to large object data.";

static PyObject *
pglarge_close(pglargeobject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method close() takes no parameters.");
		return NULL;
	}

	/* checks validity */
	if (!check_lo_obj(self, CHECK_OPEN))
		return NULL;

	/* closes large object */
	if (lo_close(self->pgcnx->cnx, self->lo_fd))
	{
		PyErr_SetString(PyExc_IOError, "error while closing large object fd.");
		return NULL;
	}
	self->lo_fd = -1;

	/* no error : returns Py_None */
	Py_INCREF(Py_None);
	return Py_None;
}

/* reads from large object */
static char pglarge_read__doc__[] =
"read(integer) -- read from large object to sized string. "
"Object must be opened in read mode before calling this method.";

static PyObject *
pglarge_read(pglargeobject * self, PyObject * args)
{
	int			size;
	PyObject   *buffer;

	/* checks validity */
	if (!check_lo_obj(self, CHECK_OPEN))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "i", &size))
	{
		PyErr_SetString(PyExc_TypeError, "read(size), wih size (integer).");
		return NULL;
	}

	if (size <= 0)
	{
		PyErr_SetString(PyExc_ValueError, "size must be positive.");
		return NULL;
	}

	/* allocate buffer and runs read */
	buffer = PyString_FromStringAndSize((char *) NULL, size);

	if ((size = lo_read(self->pgcnx->cnx, self->lo_fd, BUF(buffer), size)) < 0)
	{
		PyErr_SetString(PyExc_IOError, "error while reading.");
		Py_XDECREF(buffer);
		return NULL;
	}

	/* resize buffer and returns it */
	_PyString_Resize(&buffer, size);
	return buffer;
}

/* write to large object */
static char pglarge_write__doc__[] =
"write(string) -- write sized string to large object. "
"Object must be opened in read mode before calling this method.";

static PyObject *
pglarge_write(pglargeobject * self, PyObject * args)
{
	char	   *buffer;
	int			size,
				bufsize;

	/* checks validity */
	if (!check_lo_obj(self, CHECK_OPEN))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "s#", &buffer, &bufsize))
	{
		PyErr_SetString(PyExc_TypeError,
						"write(buffer), with buffer (sized string).");
		return NULL;
	}

	/* sends query */
	if ((size = lo_write(self->pgcnx->cnx, self->lo_fd, buffer,
						 bufsize)) < bufsize)
	{
		PyErr_SetString(PyExc_IOError, "buffer truncated during write.");
		return NULL;
	}

	/* no error : returns Py_None */
	Py_INCREF(Py_None);
	return Py_None;
}

/* go to position in large object */
static char pglarge_seek__doc__[] =
"seek(off, whence) -- move to specified position. Object must be opened "
"before calling this method. whence can be SEEK_SET, SEEK_CUR or SEEK_END, "
"constants defined by module.";

static PyObject *
pglarge_lseek(pglargeobject * self, PyObject * args)
{
	/* offset and whence are initialized to keep compiler happy */
	int			ret,
				offset = 0,
				whence = 0;

	/* checks validity */
	if (!check_lo_obj(self, CHECK_OPEN))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "ii", &offset, &whence))
	{
		PyErr_SetString(PyExc_TypeError,
			"lseek(offset, whence), with offset and whence (integers).");
		return NULL;
	}

	/* sends query */
	if ((ret = lo_lseek(self->pgcnx->cnx, self->lo_fd, offset, whence)) == -1)
	{
		PyErr_SetString(PyExc_IOError, "error while moving cursor.");
		return NULL;
	}

	/* returns position */
	return PyInt_FromLong(ret);
}

/* gets large object size */
static char pglarge_size__doc__[] =
"size() -- return large object size. "
"Object must be opened before calling this method.";

static PyObject *
pglarge_size(pglargeobject * self, PyObject * args)
{
	int			start,
				end;

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method size() takes no parameters.");
		return NULL;
	}

	/* checks validity */
	if (!check_lo_obj(self, CHECK_OPEN))
		return NULL;

	/* gets current position */
	if ((start = lo_tell(self->pgcnx->cnx, self->lo_fd)) == -1)
	{
		PyErr_SetString(PyExc_IOError, "error while getting current position.");
		return NULL;
	}

	/* gets end position */
	if ((end = lo_lseek(self->pgcnx->cnx, self->lo_fd, 0, SEEK_END)) == -1)
	{
		PyErr_SetString(PyExc_IOError, "error while getting end position.");
		return NULL;
	}

	/* move back to start position */
	if ((start = lo_lseek(self->pgcnx->cnx, self->lo_fd, start, SEEK_SET)) == -1)
	{
		PyErr_SetString(PyExc_IOError,
						"error while moving back to first position.");
		return NULL;
	}

	/* returns size */
	return PyInt_FromLong(end);
}

/* gets large object cursor position */
static char pglarge_tell__doc__[] =
"tell() -- give current position in large object. "
"Object must be opened before calling this method.";

static PyObject *
pglarge_tell(pglargeobject * self, PyObject * args)
{
	int			start;

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method tell() takes no parameters.");
		return NULL;
	}

	/* checks validity */
	if (!check_lo_obj(self, CHECK_OPEN))
		return NULL;

	/* gets current position */
	if ((start = lo_tell(self->pgcnx->cnx, self->lo_fd)) == -1)
	{
		PyErr_SetString(PyExc_IOError, "error while getting position.");
		return NULL;
	}

	/* returns size */
	return PyInt_FromLong(start);
}

/* exports large object as unix file */
static char pglarge_export__doc__[] =
"export(string) -- export large object data to specified file. "
"Object must be closed when calling this method.";

static PyObject *
pglarge_export(pglargeobject * self, PyObject * args)
{
	char	   *name;

	/* checks validity */
	if (!check_lo_obj(self, CHECK_CLOSE))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "s", &name))
	{
		PyErr_SetString(PyExc_TypeError,
						"export(filename), with filename (string).");
		return NULL;
	}

	/* runs command */
	if (!lo_export(self->pgcnx->cnx, self->lo_oid, name))
	{
		PyErr_SetString(PyExc_IOError, "error while exporting large object.");
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

/* deletes a large object */
static char pglarge_unlink__doc__[] =
"unlink() -- destroy large object. "
"Object must be closed when calling this method.";

static PyObject *
pglarge_unlink(pglargeobject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method unlink() takes no parameters.");
		return NULL;
	}

	/* checks validity */
	if (!check_lo_obj(self, CHECK_CLOSE))
		return NULL;

	/* deletes the object, invalidate it on success */
	if (!lo_unlink(self->pgcnx->cnx, self->lo_oid))
	{
		PyErr_SetString(PyExc_IOError, "error while unlinking large object");
		return NULL;
	}
	self->lo_oid = 0;

	Py_INCREF(Py_None);
	return Py_None;
}

/* large object methods */
static struct PyMethodDef pglarge_methods[] = {
	{"open", (PyCFunction) pglarge_open, 1, pglarge_open__doc__},
	{"close", (PyCFunction) pglarge_close, 1, pglarge_close__doc__},
	{"read", (PyCFunction) pglarge_read, 1, pglarge_read__doc__},
	{"write", (PyCFunction) pglarge_write, 1, pglarge_write__doc__},
	{"seek", (PyCFunction) pglarge_lseek, 1, pglarge_seek__doc__},
	{"size", (PyCFunction) pglarge_size, 1, pglarge_size__doc__},
	{"tell", (PyCFunction) pglarge_tell, 1, pglarge_tell__doc__},
	{"export", (PyCFunction) pglarge_export, 1, pglarge_export__doc__},
	{"unlink", (PyCFunction) pglarge_unlink, 1, pglarge_unlink__doc__},
	{NULL, NULL}
};

/* get attribute */
static PyObject *
pglarge_getattr(pglargeobject * self, char *name)
{
	/* list postgreSQL large object fields */

	/* associated pg connection object */
	if (!strcmp(name, "pgcnx"))
	{
		if (check_lo_obj(self, 0))
		{
			Py_INCREF(self->pgcnx);
			return (PyObject *) (self->pgcnx);
		}

		Py_INCREF(Py_None);
		return Py_None;
	}

	/* large object oid */
	if (!strcmp(name, "oid"))
	{
		if (check_lo_obj(self, 0))
			return PyInt_FromLong(self->lo_oid);

		Py_INCREF(Py_None);
		return Py_None;
	}

	/* error (status) message */
	if (!strcmp(name, "error"))
		return PyString_FromString(PQerrorMessage(self->pgcnx->cnx));

	/* attributes list */
	if (!strcmp(name, "__members__"))
	{
		PyObject   *list = PyList_New(3);

		if (list)
		{
			PyList_SET_ITEM(list, 0, PyString_FromString("oid"));
			PyList_SET_ITEM(list, 1, PyString_FromString("pgcnx"));
			PyList_SET_ITEM(list, 2, PyString_FromString("error"));
		}

		return list;
	}

	/* module name */
	if (!strcmp(name, "__module__"))
		return PyString_FromString(MODULE_NAME);

	/* class name */
	if (!strcmp(name, "__class__"))
		return PyString_FromString("pglarge");

	/* seeks name in methods (fallback) */
	return Py_FindMethod(pglarge_methods, (PyObject *) self, name);
}

/* prints query object in human readable format */
static int
pglarge_print(pglargeobject * self, FILE *fp, int flags)
{
	char		print_buffer[128];

	if (self->lo_fd >= 0)
	{
#ifdef	NO_SNPRINTF
		sprintf(print_buffer,
				"Opened large object, oid %ld", (long) self->lo_oid);
#else
		snprintf(print_buffer, sizeof(print_buffer),
				 "Opened large object, oid %ld", (long) self->lo_oid);
#endif
		fputs(print_buffer, fp);
	}
	else
	{
#ifdef	NO_SNPRINTF
		sprintf(print_buffer,
				"Closed large object, oid %ld", (long) self->lo_oid);
#else
		snprintf(print_buffer, sizeof(print_buffer),
				 "Closed large object, oid %ld", (long) self->lo_oid);
#endif
		fputs(print_buffer, fp);
	}

	return 0;
}

/* object type definition */
staticforward PyTypeObject PglargeType = {
	PyObject_HEAD_INIT(NULL)
	0,							/* ob_size */
	"pglarge",					/* tp_name */
	sizeof(pglargeobject),		/* tp_basicsize */
	0,							/* tp_itemsize */

	/* methods */
	(destructor) pglarge_dealloc,		/* tp_dealloc */
	(printfunc) pglarge_print,	/* tp_print */
	(getattrfunc) pglarge_getattr,		/* tp_getattr */
	0,							/* tp_setattr */
	0,							/* tp_compare */
	0,							/* tp_repr */
	0,							/* tp_as_number */
	0,							/* tp_as_sequence */
	0,							/* tp_as_mapping */
	0,							/* tp_hash */
};

#endif	 /* LARGE_OBJECTS */


/* --------------------------------------------------------------------- */
/* PG QUERY OBJECT IMPLEMENTATION */

/* connects to a database */
static char connect__doc__[] =
"connect(dbname, host, port, opt, tty) -- connect to a PostgreSQL database "
"using specified parameters (optionals, keywords aware).";

static PyObject *
pgconnect(pgobject * self, PyObject * args, PyObject * dict)
{
	static const char *kwlist[] = {"dbname", "host", "port", "opt",
	"tty", "user", "passwd", NULL};
	char	   *pghost,
			   *pgopt,
			   *pgtty,
			   *pgdbname,
			   *pguser,
			   *pgpasswd;
	int			pgport;
	char		port_buffer[20];
	pgobject   *npgobj;

	pghost = pgopt = pgtty = pgdbname = pguser = pgpasswd = NULL;
	pgport = -1;

	/*
	 * parses standard arguments With the right compiler warnings, this
	 * will issue a diagnostic. There is really no way around it.  If I
	 * don't declare kwlist as const char *kwlist[] then it complains when
	 * I try to assign all those constant strings to it.
	 */
	if (!PyArg_ParseTupleAndKeywords(args, dict, "|zzizzzz", (char **) kwlist,
		&pgdbname, &pghost, &pgport, &pgopt, &pgtty, &pguser, &pgpasswd))
		return NULL;

#ifdef DEFAULT_VARS
	/* handles defaults variables (for unintialised vars) */
	if ((!pghost) && (pg_default_host != Py_None))
		pghost = PyString_AsString(pg_default_host);

	if ((pgport == -1) && (pg_default_port != Py_None))
		pgport = PyInt_AsLong(pg_default_port);

	if ((!pgopt) && (pg_default_opt != Py_None))
		pgopt = PyString_AsString(pg_default_opt);

	if ((!pgtty) && (pg_default_tty != Py_None))
		pgtty = PyString_AsString(pg_default_tty);

	if ((!pgdbname) && (pg_default_base != Py_None))
		pgdbname = PyString_AsString(pg_default_base);

	if ((!pguser) && (pg_default_user != Py_None))
		pguser = PyString_AsString(pg_default_user);

	if ((!pgpasswd) && (pg_default_passwd != Py_None))
		pgpasswd = PyString_AsString(pg_default_passwd);
#endif	 /* DEFAULT_VARS */

	if ((npgobj = (pgobject *) pgobject_New()) == NULL)
		return NULL;

	if (pgport != -1)
	{
		memset(port_buffer, 0, sizeof(port_buffer));
		sprintf(port_buffer, "%d", pgport);
		npgobj->cnx = PQsetdbLogin(pghost, port_buffer, pgopt, pgtty, pgdbname,
								   pguser, pgpasswd);
	}
	else
		npgobj->cnx = PQsetdbLogin(pghost, NULL, pgopt, pgtty, pgdbname,
								   pguser, pgpasswd);

	if (PQstatus(npgobj->cnx) == CONNECTION_BAD)
	{
		PyErr_SetString(PGError, PQerrorMessage(npgobj->cnx));
		Py_XDECREF(npgobj);
		return NULL;
	}

	return (PyObject *) npgobj;
}

/* pgobject methods */

/* destructor */
static void
pg_dealloc(pgobject * self)
{
	if (self->cnx)
		PQfinish(self->cnx);

	PyMem_DEL(self);
}

/* close without deleting */
static char pg_close__doc__[] =
"close() -- close connection. All instances of the connection object and "
"derived objects (queries and large objects) can no longer be used after "
"this call.";

static PyObject *
pg_close(pgobject * self, PyObject * args)
{
	/* gets args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_TypeError, "close().");
		return NULL;
	}

	if (self->cnx)
		PQfinish(self->cnx);

	self->cnx = NULL;
	Py_INCREF(Py_None);
	return Py_None;
}

static void
pgquery_dealloc(pgqueryobject * self)
{
	if (self->last_result)
		PQclear(self->last_result);

	PyMem_DEL(self);
}

/* resets connection */
static char pg_reset__doc__[] =
"reset() -- reset connection with current parameters. All derived queries "
"and large objects derived from this connection will not be usable after "
"this call.";

static PyObject *
pg_reset(pgobject * self, PyObject * args)
{
	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method reset() takes no parameters.");
		return NULL;
	}

	/* resets the connection */
	PQreset(self->cnx);
	Py_INCREF(Py_None);
	return Py_None;
}

/* get connection socket */
static char pg_fileno__doc__[] =
"fileno() -- return database connection socket file handle.";

static PyObject *
pg_fileno(pgobject * self, PyObject * args)
{
	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method fileno() takes no parameters.");
		return NULL;
	}

#ifdef		NO_PQSOCKET
	return PyInt_FromLong((long) self->cnx->sock);
#else
	return PyInt_FromLong((long) PQsocket(self->cnx));
#endif
}

/* get number of rows */
static char pgquery_ntuples__doc__[] =
"ntuples() -- returns number of tuples returned by query.";

static PyObject *
pgquery_ntuples(pgqueryobject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method ntuples() takes no parameters.");
		return NULL;
	}

	return PyInt_FromLong((long) PQntuples(self->last_result));
}

/* list fields names from query result */
static char pgquery_listfields__doc__[] =
"listfields() -- Lists field names from result.";

static PyObject *
pgquery_listfields(pgqueryobject * self, PyObject * args)
{
	int			i,
				n;
	char	   *name;
	PyObject   *fieldstuple,
			   *str;

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method listfields() takes no parameters.");
		return NULL;
	}

	/* builds tuple */
	n = PQnfields(self->last_result);
	fieldstuple = PyTuple_New(n);

	for (i = 0; i < n; i++)
	{
		name = PQfname(self->last_result, i);
		str = PyString_FromString(name);
		PyTuple_SET_ITEM(fieldstuple, i, str);
	}

	return fieldstuple;
}

/* get field name from last result */
static char pgquery_fieldname__doc__[] =
"fieldname() -- returns name of field from result from its position.";

static PyObject *
pgquery_fieldname(pgqueryobject * self, PyObject * args)
{
	int			i;
	char	   *name;

	/* gets args */
	if (!PyArg_ParseTuple(args, "i", &i))
	{
		PyErr_SetString(PyExc_TypeError,
						"fieldname(number), with number(integer).");
		return NULL;
	}

	/* checks number validity */
	if (i >= PQnfields(self->last_result))
	{
		PyErr_SetString(PyExc_ValueError, "invalid field number.");
		return NULL;
	}

	/* gets fields name and builds object */
	name = PQfname(self->last_result, i);
	return PyString_FromString(name);
}

/* gets fields number from name in last result */
static char pgquery_fieldnum__doc__[] =
"fieldnum() -- returns position in query for field from its name.";

static PyObject *
pgquery_fieldnum(pgqueryobject * self, PyObject * args)
{
	char	   *name;
	int			num;

	/* gets args */
	if (!PyArg_ParseTuple(args, "s", &name))
	{
		PyErr_SetString(PyExc_TypeError, "fieldnum(name), with name (string).");
		return NULL;
	}

	/* gets field number */
	if ((num = PQfnumber(self->last_result, name)) == -1)
	{
		PyErr_SetString(PyExc_ValueError, "Unknown field.");
		return NULL;
	}

	return PyInt_FromLong(num);
}

/* retrieves last result */
static char pgquery_getresult__doc__[] =
"getresult() -- Gets the result of a query.  The result is returned "
"as a list of rows, each one a list of fields in the order returned "
"by the server.";

static PyObject *
pgquery_getresult(pgqueryobject * self, PyObject * args)
{
	PyObject   *rowtuple,
			   *reslist,
			   *val;
	int			i,
				j,
				m,
				n,
			   *typ;

	/* checks args (args == NULL for an internal call) */
	if ((args != NULL) && (!PyArg_ParseTuple(args, "")))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method getresult() takes no parameters.");
		return NULL;
	}

	/* stores result in tuple */
	m = PQntuples(self->last_result);
	n = PQnfields(self->last_result);
	reslist = PyList_New(m);

	if ((typ = malloc(sizeof(int) * n)) == NULL)
	{
		PyErr_SetString(PyExc_SyntaxError, "memory error in getresult().");
		return NULL;
	}

	for (j = 0; j < n; j++)
	{
		switch (PQftype(self->last_result, j))
		{
			case INT2OID:
			case INT4OID:
			case OIDOID:
				typ[j] = 1;
				break;

			case FLOAT4OID:
			case FLOAT8OID:
				typ[j] = 2;
				break;

			case CASHOID:
				typ[j] = 3;
				break;

			default:
				typ[j] = 4;
				break;
		}
	}

	for (i = 0; i < m; i++)
	{
		if ((rowtuple = PyTuple_New(n)) == NULL)
		{
			Py_DECREF(reslist);
			reslist = NULL;
			goto exit;
		}

		for (j = 0; j < n; j++)
		{
			int			k;
			char	   *s = PQgetvalue(self->last_result, i, j);
			char		cashbuf[64];

			if (PQgetisnull(self->last_result, i, j))
			{
				Py_INCREF(Py_None);
				val = Py_None;
			}
			else
				switch (typ[j])
				{
					case 1:
						val = PyInt_FromLong(strtol(s, NULL, 10));
						break;

					case 2:
						val = PyFloat_FromDouble(strtod(s, NULL));
						break;

					case 3:		/* get rid of the '$' and commas */
						if (*s == '$')	/* there's talk of getting rid of
										 * it */
							s++;

						if ((s[0] == '-' || s[0] == '(') && s[1] == '$')
							*(++s) = '-';

						for (k = 0; *s; s++)
							if (*s != ',')
								cashbuf[k++] = *s;

						cashbuf[k] = 0;
						val = PyFloat_FromDouble(strtod(cashbuf, NULL));
						break;

					default:
						val = PyString_FromString(s);
						break;
				}

			if (val == NULL)
			{
				Py_DECREF(reslist);
				Py_DECREF(rowtuple);
				reslist = NULL;
				goto exit;
			}

			PyTuple_SET_ITEM(rowtuple, j, val);
		}

		PyList_SET_ITEM(reslist, i, rowtuple);
	}

exit:
	free(typ);

	/* returns list */
	return reslist;
}

/* retrieves last result as a list of dictionaries*/
static char pgquery_dictresult__doc__[] =
"dictresult() -- Gets the result of a query.  The result is returned "
"as a list of rows, each one a dictionary with the field names used "
"as the labels.";

static PyObject *
pgquery_dictresult(pgqueryobject * self, PyObject * args)
{
	PyObject   *dict,
			   *reslist,
			   *val;
	int			i,
				j,
				m,
				n,
			   *typ;

	/* checks args (args == NULL for an internal call) */
	if ((args != NULL) && (!PyArg_ParseTuple(args, "")))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method getresult() takes no parameters.");
		return NULL;
	}

	/* stores result in list */
	m = PQntuples(self->last_result);
	n = PQnfields(self->last_result);
	reslist = PyList_New(m);

	if ((typ = malloc(sizeof(int) * n)) == NULL)
	{
		PyErr_SetString(PyExc_SyntaxError, "memory error in dictresult().");
		return NULL;
	}

	for (j = 0; j < n; j++)
	{
		switch (PQftype(self->last_result, j))
		{
			case INT2OID:
			case INT4OID:
			case OIDOID:
				typ[j] = 1;
				break;

			case FLOAT4OID:
			case FLOAT8OID:
				typ[j] = 2;
				break;

			case CASHOID:
				typ[j] = 3;
				break;

			default:
				typ[j] = 4;
				break;
		}
	}

	for (i = 0; i < m; i++)
	{
		if ((dict = PyDict_New()) == NULL)
		{
			Py_DECREF(reslist);
			reslist = NULL;
			goto exit;
		}

		for (j = 0; j < n; j++)
		{
			int			k;
			char	   *s = PQgetvalue(self->last_result, i, j);
			char		cashbuf[64];

			if (PQgetisnull(self->last_result, i, j))
			{
				Py_INCREF(Py_None);
				val = Py_None;
			}
			else
				switch (typ[j])
				{
					case 1:
						val = PyInt_FromLong(strtol(s, NULL, 10));
						break;

					case 2:
						val = PyFloat_FromDouble(strtod(s, NULL));
						break;

					case 3:		/* get rid of the '$' and commas */
						if (*s == '$')	/* there's talk of getting rid of
										 * it */
							s++;

						if ((s[0] == '-' || s[0] == '(') && s[1] == '$')
							*(++s) = '-';

						for (k = 0; *s; s++)
							if (*s != ',')
								cashbuf[k++] = *s;

						cashbuf[k] = 0;
						val = PyFloat_FromDouble(strtod(cashbuf, NULL));
						break;

					default:
						val = PyString_FromString(s);
						break;
				}

			if (val == NULL)
			{
				Py_DECREF(dict);
				Py_DECREF(reslist);
				reslist = NULL;
				goto exit;
			}

			PyDict_SetItemString(dict, PQfname(self->last_result, j), val);
			Py_DECREF(val);
		}

		PyList_SET_ITEM(reslist, i, dict);
	}

exit:
	free(typ);

	/* returns list */
	return reslist;
}

/* gets asynchronous notify */
static char pg_getnotify__doc__[] =
"getnotify() -- get database notify for this connection.";

static PyObject *
pg_getnotify(pgobject * self, PyObject * args)
{
	PGnotify   *notify;
	PGresult   *result;
	PyObject   *notify_result,
			   *temp;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method getnotify() takes no parameters.");
		return NULL;
	}

	/* gets notify and builds result */

	/*
	 * notifies only come back as result of a query, so I send an empty
	 * query
	 */
	Py_BEGIN_ALLOW_THREADS
		result = PQexec(self->cnx, " ");
	Py_END_ALLOW_THREADS

		if ((notify = PQnotifies(self->cnx)) != NULL)
	{
		if ((notify_result = PyTuple_New(2)) == NULL)
		{
			PQclear(result);
			return NULL;
		}

		if ((temp = PyString_FromString(notify->relname)) == NULL)
		{
			PQclear(result);
			return NULL;
		}

		PyTuple_SET_ITEM(notify_result, 0, temp);

		if ((temp = PyInt_FromLong(notify->be_pid)) == NULL)
		{
			PQclear(result);
			Py_DECREF(notify_result);
			return NULL;
		}

		PyTuple_SET_ITEM(notify_result, 1, temp);
		free(notify);
	}
	else
	{
		Py_INCREF(Py_None);
		notify_result = Py_None;
	}

	PQclear(result);

	/* returns result */
	return notify_result;
}

/* source creation */
static char pg_source__doc__[] =
"source() -- creates a new source object for this connection";

static PyObject *
pg_source(pgobject * self, PyObject * args)
{
	/* checks validity */
	if (!check_cnx_obj(self))
		return NULL;

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_TypeError, "method source() takes no parameter.");
		return NULL;
	}

	/* allocate new pg query object */
	return (PyObject *) pgsource_new(self);
}

/* database query */
static char pg_query__doc__[] =
"query(sql) -- creates a new query object for this connection,"
" using sql (string) request.";

static PyObject *
pg_query(pgobject * self, PyObject * args)
{
	char	   *query;
	PGresult   *result;
	pgqueryobject *npgobj;
	int			status;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* get query args */
	if (!PyArg_ParseTuple(args, "s", &query))
	{
		PyErr_SetString(PyExc_TypeError, "query(sql), with sql (string).");
		return NULL;
	}

	/* frees previous result */
	if (self->last_result)
	{
		PQclear(self->last_result);
		self->last_result = NULL;
	}

	/* gets result */
	Py_BEGIN_ALLOW_THREADS
		result = PQexec(self->cnx, query);
	Py_END_ALLOW_THREADS

	/* checks result validity */
		if (!result)
	{
		PyErr_SetString(PyExc_ValueError, PQerrorMessage(self->cnx));
		return NULL;
	}

	/* checks result status */
	if ((status = PQresultStatus(result)) != PGRES_TUPLES_OK)
	{
		Oid			oid;

		PQclear(result);

		switch (status)
		{
			case PGRES_EMPTY_QUERY:
				PyErr_SetString(PyExc_ValueError, "empty query.");
				break;
			case PGRES_BAD_RESPONSE:
			case PGRES_FATAL_ERROR:
			case PGRES_NONFATAL_ERROR:
				PyErr_SetString(PGError, PQerrorMessage(self->cnx));
				break;
			case PGRES_COMMAND_OK:		/* could be an INSERT */
				if ((oid = PQoidValue(result)) == InvalidOid)	/* nope */
				{
					Py_INCREF(Py_None);
					return Py_None;
				}

				/* otherwise, return the oid */
				return PyInt_FromLong(oid);

			case PGRES_COPY_OUT:		/* no data will be received */
			case PGRES_COPY_IN:
				Py_INCREF(Py_None);
				return Py_None;
			default:
				PyErr_SetString(PGError, "internal error: "
								"unknown result status.");
				break;
		}

		return NULL;			/* error detected on query */
	}

	if ((npgobj = PyObject_NEW(pgqueryobject, &PgQueryType)) == NULL)
		return NULL;

	/* stores result and returns object */
	npgobj->last_result = result;
	return (PyObject *) npgobj;
}

#ifdef DIRECT_ACCESS
static char pg_putline__doc__[] =
"putline() -- sends a line directly to the backend";

/* direct acces function : putline */
static PyObject *
pg_putline(pgobject * self, PyObject * args)
{
	char	   *line;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* reads args */
	if (!PyArg_ParseTuple(args, "s", &line))
	{
		PyErr_SetString(PyExc_TypeError, "putline(line), with line (string).");
		return NULL;
	}

	/* sends line to backend */
	PQputline(self->cnx, line);
	Py_INCREF(Py_None);
	return Py_None;
}

/* direct access function : getline */
static char pg_getline__doc__[] =
"getline() -- gets a line directly from the backend.";

static PyObject *
pg_getline(pgobject * self, PyObject * args)
{
	char		line[MAX_BUFFER_SIZE];
	PyObject   *str = NULL;		/* GCC */

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method getline() takes no parameters.");
		return NULL;
	}

	/* gets line */
	switch (PQgetline(self->cnx, line, MAX_BUFFER_SIZE))
	{
		case 0:
			str = PyString_FromString(line);
			break;
		case 1:
			PyErr_SetString(PyExc_MemoryError, "buffer overflow");
			str = NULL;
			break;
		case EOF:
			Py_INCREF(Py_None);
			str = Py_None;
			break;
	}

	return str;
}

/* direct access function : end copy */
static char pg_endcopy__doc__[] =
"endcopy() -- synchronizes client and server";

static PyObject *
pg_endcopy(pgobject * self, PyObject * args)
{
	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method endcopy() takes no parameters.");
		return NULL;
	}

	/* ends direct copy */
	PQendcopy(self->cnx);
	Py_INCREF(Py_None);
	return Py_None;
}

#endif	 /* DIRECT_ACCESS */


static PyObject *
pgquery_print(pgqueryobject * self, FILE *fp, int flags)
{
	PQprintOpt	op;

	memset(&op, 0, sizeof(op));
	op.align = 1;
	op.header = 1;
	op.fieldSep = "|";
	op.pager = 1;
	PQprint(fp, self->last_result, &op);
	return 0;
}

static PyObject *
pgquery_repr(pgqueryobject * self)
{
	return PyString_FromString("<pg query result>");
}

/* insert table */
static char pg_inserttable__doc__[] =
"inserttable(string, list) -- insert list in table. The fields in the "
"list must be in the same order as in the table.";

static PyObject *
pg_inserttable(pgobject * self, PyObject * args)
{
	PGresult   *result;
	char	   *table,
			   *buffer,
			   *temp;
	char		temp_buffer[256];
	PyObject   *list,
			   *sublist,
			   *item;
	PyObject   *(*getitem) (PyObject *, int);
	PyObject   *(*getsubitem) (PyObject *, int);
	int			i,
				j;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "sO:filter", &table, &list))
	{
		PyErr_SetString(PyExc_TypeError,
					  "tableinsert(table, content), with table (string) "
						"and content (list).");
		return NULL;
	}

	/* checks list type */
	if (PyTuple_Check(list))
		getitem = PyTuple_GetItem;
	else if (PyList_Check(list))
		getitem = PyList_GetItem;
	else
	{
		PyErr_SetString(PyExc_TypeError,
						"second arg must be some kind of array.");
		return NULL;
	}

	/* checks sublists type */
	for (i = 0; (sublist = getitem(list, i)) != NULL; i++)
	{
		if (!PyTuple_Check(sublist) && !PyList_Check(sublist))
		{
			PyErr_SetString(PyExc_TypeError,
						 "second arg must contain some kind of arrays.");
			return NULL;
		}
	}

	/* allocate buffer */
	if (!(buffer = malloc(MAX_BUFFER_SIZE)))
	{
		PyErr_SetString(PyExc_MemoryError, "can't allocate insert buffer.");
		return NULL;
	}

	/* starts query */
	sprintf(buffer, "copy %s from stdin", table);

	Py_BEGIN_ALLOW_THREADS
		result = PQexec(self->cnx, buffer);
	Py_END_ALLOW_THREADS

		if (!result)
	{
		free(buffer);
		PyErr_SetString(PyExc_ValueError, PQerrorMessage(self->cnx));
		return NULL;
	}

	PQclear(result);

	/* feeds table */
	for (i = 0; (sublist = getitem(list, i)) != NULL; i++)
	{
		if (PyTuple_Check(sublist))
			getsubitem = PyTuple_GetItem;
		else
			getsubitem = PyList_GetItem;

		/* builds insert line */
		buffer[0] = 0;

		for (j = 0; (item = getsubitem(sublist, j)) != NULL; j++)
		{
			/* converts item to string */
			if (PyString_Check(item))
				temp = PyString_AS_STRING(item);
			else if (PyInt_Check(item))
			{
				long		k;

				k = PyInt_AsLong(item);
				sprintf(temp_buffer, "%ld", k);
				temp = temp_buffer;
			}
			else if (PyLong_Check(item))
			{
				long		k;

				k = PyLong_AsLong(item);
				sprintf(temp_buffer, "%ld", k);
				temp = temp_buffer;
			}
			else if (PyFloat_Check(item))
			{
				double		k;

				k = PyFloat_AS_DOUBLE(item);
				sprintf(temp_buffer, "%g", k);
				temp = temp_buffer;
			}
			else
			{
				free(buffer);
				PyErr_SetString(PyExc_ValueError,
								"items must be strings, integers, "
								"longs or double (real).");
				return NULL;
			}

			/* concats buffer */
			if (strlen(buffer))
				strncat(buffer, "\t", MAX_BUFFER_SIZE - strlen(buffer));

			strncat(buffer, temp, MAX_BUFFER_SIZE - strlen(buffer));
		}

		strncat(buffer, "\n", MAX_BUFFER_SIZE - strlen(buffer));

		/* sends data */
		PQputline(self->cnx, buffer);
	}

	/* ends query */
	PQputline(self->cnx, "\\.\n");
	PQendcopy(self->cnx);
	free(buffer);

	/* no error : returns nothing */
	Py_INCREF(Py_None);
	return Py_None;
}


/* creates large object */
static char pg_locreate__doc__[] =
"locreate() -- creates a new large object in the database.";

static PyObject *
pg_locreate(pgobject * self, PyObject * args)
{
	int			mode;
	Oid			lo_oid;

	/* checks validity */
	if (!check_cnx_obj(self))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "i", &mode))
	{
		PyErr_SetString(PyExc_TypeError,
						"locreate(mode), with mode (integer).");
		return NULL;
	}

	/* creates large object */
	lo_oid = lo_creat(self->cnx, mode);
	if (lo_oid == 0)
	{
		PyErr_SetString(PGError, "can't create large object.");
		return NULL;
	}

	return (PyObject *) pglarge_new(self, lo_oid);
}

/* init from already known oid */
static char pg_getlo__doc__[] =
"getlo(long) -- create a large object instance for the specified oid.";

static PyObject *
pg_getlo(pgobject * self, PyObject * args)
{
	int			lo_oid;

	/* checks validity */
	if (!check_cnx_obj(self))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "i", &lo_oid))
	{
		PyErr_SetString(PyExc_TypeError, "getlo(oid), with oid (integer).");
		return NULL;
	}

	if (!lo_oid)
	{
		PyErr_SetString(PyExc_ValueError, "the object oid can't be null.");
		return NULL;
	}

	/* creates object */
	return (PyObject *) pglarge_new(self, lo_oid);
}

/* import unix file */
static char pg_loimport__doc__[] =
"loimport(string) -- create a new large object from specified file.";

static PyObject *
pg_loimport(pgobject * self, PyObject * args)
{
	char	   *name;
	Oid			lo_oid;

	/* checks validity */
	if (!check_cnx_obj(self))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "s", &name))
	{
		PyErr_SetString(PyExc_TypeError, "loimport(name), with name (string).");
		return NULL;
	}

	/* imports file and checks result */
	lo_oid = lo_import(self->cnx, name);
	if (lo_oid == 0)
	{
		PyErr_SetString(PGError, "can't create large object.");
		return NULL;
	}

	return (PyObject *) pglarge_new(self, lo_oid);
}


/* connection object methods */
static struct PyMethodDef pgobj_methods[] = {
	{"source", (PyCFunction) pg_source, 1, pg_source__doc__},
	{"query", (PyCFunction) pg_query, 1, pg_query__doc__},
	{"reset", (PyCFunction) pg_reset, 1, pg_reset__doc__},
	{"close", (PyCFunction) pg_close, 1, pg_close__doc__},
	{"fileno", (PyCFunction) pg_fileno, 1, pg_fileno__doc__},
	{"getnotify", (PyCFunction) pg_getnotify, 1, pg_getnotify__doc__},
	{"inserttable", (PyCFunction) pg_inserttable, 1, pg_inserttable__doc__},

#ifdef DIRECT_ACCESS
	{"putline", (PyCFunction) pg_putline, 1, pg_putline__doc__},
	{"getline", (PyCFunction) pg_getline, 1, pg_getline__doc__},
	{"endcopy", (PyCFunction) pg_endcopy, 1, pg_endcopy__doc__},
#endif	 /* DIRECT_ACCESS */

#ifdef LARGE_OBJECTS
	{"locreate", (PyCFunction) pg_locreate, 1, pg_locreate__doc__},
	{"getlo", (PyCFunction) pg_getlo, 1, pg_getlo__doc__},
	{"loimport", (PyCFunction) pg_loimport, 1, pg_loimport__doc__},
#endif	 /* LARGE_OBJECTS */

	{NULL, NULL}				/* sentinel */
};

/* get attribute */
static PyObject *
pg_getattr(pgobject * self, char *name)
{

	/*
	 * Although we could check individually, there are only a few
	 * attributes that don't require a live connection and unless someone
	 * has an urgent need, this will have to do
	 */
	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* list postgreSQL connection fields */

	/* postmaster host */
	if (!strcmp(name, "host"))
	{
		char	   *r = PQhost(self->cnx);

		return r ? PyString_FromString(r) : PyString_FromString("localhost");
	}

	/* postmaster port */
	if (!strcmp(name, "port"))
		return PyInt_FromLong(atol(PQport(self->cnx)));

	/* selected database */
	if (!strcmp(name, "db"))
		return PyString_FromString(PQdb(self->cnx));

	/* selected options */
	if (!strcmp(name, "options"))
		return PyString_FromString(PQoptions(self->cnx));

	/* selected postgres tty */
	if (!strcmp(name, "tty"))
		return PyString_FromString(PQtty(self->cnx));

	/* error (status) message */
	if (!strcmp(name, "error"))
		return PyString_FromString(PQerrorMessage(self->cnx));

	/* connection status : 1 - OK, 0 - BAD */
	if (!strcmp(name, "status"))
		return PyInt_FromLong(PQstatus(self->cnx) == CONNECTION_OK ? 1 : 0);

	/* provided user name */
	if (!strcmp(name, "user"))
		return PyString_FromString("Deprecated facility");
	/* return PyString_FromString(fe_getauthname("<unknown user>")); */

	/* attributes list */
	if (!strcmp(name, "__members__"))
	{
		PyObject   *list = PyList_New(8);

		if (list)
		{
			PyList_SET_ITEM(list, 0, PyString_FromString("host"));
			PyList_SET_ITEM(list, 1, PyString_FromString("port"));
			PyList_SET_ITEM(list, 2, PyString_FromString("db"));
			PyList_SET_ITEM(list, 3, PyString_FromString("options"));
			PyList_SET_ITEM(list, 4, PyString_FromString("tty"));
			PyList_SET_ITEM(list, 5, PyString_FromString("error"));
			PyList_SET_ITEM(list, 6, PyString_FromString("status"));
			PyList_SET_ITEM(list, 7, PyString_FromString("user"));
		}

		return list;
	}

	return Py_FindMethod(pgobj_methods, (PyObject *) self, name);
}

/* object type definition */
staticforward PyTypeObject PgType = {
	PyObject_HEAD_INIT(NULL)
	0,							/* ob_size */
	"pgobject",					/* tp_name */
	sizeof(pgobject),			/* tp_basicsize */
	0,							/* tp_itemsize */
	/* methods */
	(destructor) pg_dealloc,	/* tp_dealloc */
	0,							/* tp_print */
	(getattrfunc) pg_getattr,	/* tp_getattr */
	0,							/* tp_setattr */
	0,							/* tp_compare */
	0,							/* tp_repr */
	0,							/* tp_as_number */
	0,							/* tp_as_sequence */
	0,							/* tp_as_mapping */
	0,							/* tp_hash */
};


/* query object methods */
static struct PyMethodDef pgquery_methods[] = {
	{"getresult", (PyCFunction) pgquery_getresult, 1, pgquery_getresult__doc__},
	{"dictresult", (PyCFunction) pgquery_dictresult, 1, pgquery_dictresult__doc__},
	{"fieldname", (PyCFunction) pgquery_fieldname, 1, pgquery_fieldname__doc__},
	{"fieldnum", (PyCFunction) pgquery_fieldnum, 1, pgquery_fieldnum__doc__},
	{"listfields", (PyCFunction) pgquery_listfields, 1, pgquery_listfields__doc__},
	{"ntuples", (PyCFunction) pgquery_ntuples, 1, pgquery_ntuples__doc__},
	{NULL, NULL}
};

/* gets query object attributes */
static PyObject *
pgquery_getattr(pgqueryobject * self, char *name)
{
	/* list postgreSQL connection fields */
	return Py_FindMethod(pgquery_methods, (PyObject *) self, name);
}

/* query type definition */
staticforward PyTypeObject PgQueryType = {
	PyObject_HEAD_INIT(NULL)
	0,							/* ob_size */
	"pgqueryobject",			/* tp_name */
	sizeof(pgqueryobject),		/* tp_basicsize */
	0,							/* tp_itemsize */
	/* methods */
	(destructor) pgquery_dealloc,		/* tp_dealloc */
	(printfunc) pgquery_print,	/* tp_print */
	(getattrfunc) pgquery_getattr,		/* tp_getattr */
	0,							/* tp_setattr */
	0,							/* tp_compare */
	(reprfunc) pgquery_repr,	/* tp_repr */
	0,							/* tp_as_number */
	0,							/* tp_as_sequence */
	0,							/* tp_as_mapping */
	0,							/* tp_hash */
};



/* --------------------------------------------------------------------- */

/* MODULE FUNCTIONS */

#ifdef DEFAULT_VARS

/* gets default host */
static char getdefhost__doc__[] =
"get_defhost() -- return default database host.";

static PyObject *
pggetdefhost(PyObject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method get_defhost() takes no parameter.");
		return NULL;
	}

	Py_XINCREF(pg_default_host);
	return pg_default_host;
}

/* sets default host */
static char setdefhost__doc__[] =
"set_defhost(string) -- set default database host. Return previous value.";

static PyObject *
pgsetdefhost(PyObject * self, PyObject * args)
{
	char	   *temp = NULL;
	PyObject   *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
						"set_defhost(name), with name (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_host;

	if (temp)
		pg_default_host = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_host = Py_None;
	}

	return old;
}

/* gets default base */
static char getdefbase__doc__[] =
"get_defbase() -- return default database name.";

static PyObject *
pggetdefbase(PyObject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method get_defbase() takes no parameter.");
		return NULL;
	}

	Py_XINCREF(pg_default_base);
	return pg_default_base;
}

/* sets default base */
static char setdefbase__doc__[] =
"set_defbase(string) -- set default database name. Return previous value";

static PyObject *
pgsetdefbase(PyObject * self, PyObject * args)
{
	char	   *temp = NULL;
	PyObject   *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
						"set_defbase(name), with name (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_base;

	if (temp)
		pg_default_base = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_base = Py_None;
	}

	return old;
}

/* gets default options */
static char getdefopt__doc__[] =
"get_defopt() -- return default database options.";

static PyObject *
pggetdefopt(PyObject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method get_defopt() takes no parameter.");
		return NULL;
	}

	Py_XINCREF(pg_default_opt);
	return pg_default_opt;
}

/* sets default opt */
static char setdefopt__doc__[] =
"set_defopt(string) -- set default database options. Return previous value.";

static PyObject *
pgsetdefopt(PyObject * self, PyObject * args)
{
	char	   *temp = NULL;
	PyObject   *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
						"set_defopt(name), with name (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_opt;

	if (temp)
		pg_default_opt = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_opt = Py_None;
	}

	return old;
}

/* gets default tty */
static char getdeftty__doc__[] =
"get_deftty() -- return default database debug terminal.";

static PyObject *
pggetdeftty(PyObject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method get_deftty() takes no parameter.");
		return NULL;
	}

	Py_XINCREF(pg_default_tty);
	return pg_default_tty;
}

/* sets default tty */
static char setdeftty__doc__[] =
"set_deftty(string) -- set default database debug terminal. "
"Return previous value.";

static PyObject *
pgsetdeftty(PyObject * self, PyObject * args)
{
	char	   *temp = NULL;
	PyObject   *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
						"set_deftty(name), with name (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_tty;

	if (temp)
		pg_default_tty = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_tty = Py_None;
	}

	return old;
}

/* gets default username */
static char getdefuser__doc__[] =
"get_defuser() -- return default database username.";

static PyObject *
pggetdefuser(PyObject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method get_defuser() takes no parameter.");

		return NULL;
	}

	Py_XINCREF(pg_default_user);
	return pg_default_user;
}

/* sets default username */
static char setdefuser__doc__[] =
"set_defuser() -- set default database username. Return previous value.";

static PyObject *
pgsetdefuser(PyObject * self, PyObject * args)
{
	char	   *temp = NULL;
	PyObject   *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
						"set_defuser(name), with name (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_user;

	if (temp)
		pg_default_user = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_user = Py_None;
	}

	return old;
}

/* sets default password */
static char setdefpasswd__doc__[] =
"set_defpasswd() -- set default database password.";

static PyObject *
pgsetdefpasswd(PyObject * self, PyObject * args)
{
	char	   *temp = NULL;
	PyObject   *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
				"set_defpasswd(password), with password (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_passwd;

	if (temp)
		pg_default_passwd = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_passwd = Py_None;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

/* gets default port */
static char getdefport__doc__[] =
"get_defport() -- return default database port.";

static PyObject *
pggetdefport(PyObject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
						"method get_defport() takes no parameter.");
		return NULL;
	}

	Py_XINCREF(pg_default_port);
	return pg_default_port;
}

/* sets default port */
static char setdefport__doc__[] =
"set_defport(integer) -- set default database port. Return previous value.";

static PyObject *
pgsetdefport(PyObject * self, PyObject * args)
{
	long int	port = -2;
	PyObject   *old;

	/* gets arguments */
	if ((!PyArg_ParseTuple(args, "l", &port)) || (port < -1))
	{
		PyErr_SetString(PyExc_TypeError, "set_defport(port), with port "
						"(positive integer/-1).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_port;

	if (port != -1)
		pg_default_port = PyLong_FromLong(port);
	else
	{
		Py_INCREF(Py_None);
		pg_default_port = Py_None;
	}

	return old;
}

#endif	 /* DEFAULT_VARS */

/* List of functions defined in the module */

static struct PyMethodDef pg_methods[] = {
	{"connect", (PyCFunction) pgconnect, 3, connect__doc__},

#ifdef DEFAULT_VARS
	{"get_defhost", pggetdefhost, 1, getdefhost__doc__},
	{"set_defhost", pgsetdefhost, 1, setdefhost__doc__},
	{"get_defbase", pggetdefbase, 1, getdefbase__doc__},
	{"set_defbase", pgsetdefbase, 1, setdefbase__doc__},
	{"get_defopt", pggetdefopt, 1, getdefopt__doc__},
	{"set_defopt", pgsetdefopt, 1, setdefopt__doc__},
	{"get_deftty", pggetdeftty, 1, getdeftty__doc__},
	{"set_deftty", pgsetdeftty, 1, setdeftty__doc__},
	{"get_defport", pggetdefport, 1, getdefport__doc__},
	{"set_defport", pgsetdefport, 1, setdefport__doc__},
	{"get_defuser", pggetdefuser, 1, getdefuser__doc__},
	{"set_defuser", pgsetdefuser, 1, setdefuser__doc__},
	{"set_defpasswd", pgsetdefpasswd, 1, setdefpasswd__doc__},
#endif	 /* DEFAULT_VARS */
	{NULL, NULL}				/* sentinel */
};

static char pg__doc__[] = "Python interface to PostgreSQL DB";

/* Initialization function for the module */
DL_EXPORT(void)
init_pg(void)
{
	PyObject   *mod,
			   *dict,
			   *v;

	/* Initialize here because some WIN platforms get confused otherwise */
	PglargeType.ob_type = PgType.ob_type = PgQueryType.ob_type =
		PgSourceType.ob_type = &PyType_Type;

	/* Create the module and add the functions */
	mod = Py_InitModule4("_pg", pg_methods, pg__doc__, NULL, PYTHON_API_VERSION);
	dict = PyModule_GetDict(mod);

	/* Add some symbolic constants to the module */
	PGError = PyString_FromString("_pg.error");
	PyDict_SetItemString(dict, "error", PGError);

	/* Make the version available */
	v = PyString_FromString(PyPgVersion);
	PyDict_SetItemString(dict, "version", v);
	PyDict_SetItemString(dict, "__version__", v);
	Py_DECREF(v);

	/* results type for queries */
	PyDict_SetItemString(dict, "RESULT_EMPTY", PyInt_FromLong(RESULT_EMPTY));
	PyDict_SetItemString(dict, "RESULT_DML", PyInt_FromLong(RESULT_DML));
	PyDict_SetItemString(dict, "RESULT_DDL", PyInt_FromLong(RESULT_DDL));
	PyDict_SetItemString(dict, "RESULT_DQL", PyInt_FromLong(RESULT_DQL));

#ifdef LARGE_OBJECTS
	/* create mode for large objects */
	PyDict_SetItemString(dict, "INV_READ", PyInt_FromLong(INV_READ));
	PyDict_SetItemString(dict, "INV_WRITE", PyInt_FromLong(INV_WRITE));

	/* position flags for lo_lseek */
	PyDict_SetItemString(dict, "SEEK_SET", PyInt_FromLong(SEEK_SET));
	PyDict_SetItemString(dict, "SEEK_CUR", PyInt_FromLong(SEEK_CUR));
	PyDict_SetItemString(dict, "SEEK_END", PyInt_FromLong(SEEK_END));
#endif	 /* LARGE_OBJECTS */

#ifdef DEFAULT_VARS
	/* prepares default values */
	Py_INCREF(Py_None);
	pg_default_host = Py_None;
	Py_INCREF(Py_None);
	pg_default_base = Py_None;
	Py_INCREF(Py_None);
	pg_default_opt = Py_None;
	Py_INCREF(Py_None);
	pg_default_port = Py_None;
	Py_INCREF(Py_None);
	pg_default_tty = Py_None;
	Py_INCREF(Py_None);
	pg_default_user = Py_None;
	Py_INCREF(Py_None);
	pg_default_passwd = Py_None;
#endif	 /* DEFAULT_VARS */

	/* Check for errors */
	if (PyErr_Occurred())
		Py_FatalError("can't initialize module _pg");
}
