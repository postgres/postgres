/*
 * reporting Python exceptions as PostgreSQL errors
 *
 * src/pl/plpython/plpy_elog.c
 */

#include "postgres.h"

#include "lib/stringinfo.h"

#include "plpython.h"

#include "plpy_elog.h"

#include "plpy_main.h"
#include "plpy_procedure.h"


PyObject   *PLy_exc_error = NULL;
PyObject   *PLy_exc_fatal = NULL;
PyObject   *PLy_exc_spi_error = NULL;


static void PLy_traceback(PyObject *e, PyObject *v, PyObject *tb,
			  char **xmsg, char **tbmsg, int *tb_depth);
static void PLy_get_spi_error_data(PyObject *exc, int *sqlerrcode, char **detail,
					   char **hint, char **query, int *position,
				   char **schema_name, char **table_name, char **column_name,
					   char **datatype_name, char **constraint_name);
static void PLy_get_error_data(PyObject *exc, int *sqlerrcode, char **detail,
	  char **hint, char **schema_name, char **table_name, char **column_name,
				   char **datatype_name, char **constraint_name);
static char *get_source_line(const char *src, int lineno);

static void get_string_attr(PyObject *obj, char *attrname, char **str);
static bool set_string_attr(PyObject *obj, char *attrname, char *str);

/*
 * Emit a PG error or notice, together with any available info about
 * the current Python error, previously set by PLy_exception_set().
 * This should be used to propagate Python errors into PG.  If fmt is
 * NULL, the Python error becomes the primary error message, otherwise
 * it becomes the detail.  If there is a Python traceback, it is put
 * in the context.
 */
void
PLy_elog(int elevel, const char *fmt,...)
{
	char	   *xmsg;
	char	   *tbmsg;
	int			tb_depth;
	StringInfoData emsg;
	PyObject   *exc,
			   *val,
			   *tb;
	const char *primary = NULL;
	int			sqlerrcode = 0;
	char	   *detail = NULL;
	char	   *hint = NULL;
	char	   *query = NULL;
	int			position = 0;
	char	   *schema_name = NULL;
	char	   *table_name = NULL;
	char	   *column_name = NULL;
	char	   *datatype_name = NULL;
	char	   *constraint_name = NULL;

	PyErr_Fetch(&exc, &val, &tb);

	if (exc != NULL)
	{
		PyErr_NormalizeException(&exc, &val, &tb);

		if (PyErr_GivenExceptionMatches(val, PLy_exc_spi_error))
			PLy_get_spi_error_data(val, &sqlerrcode,
								   &detail, &hint, &query, &position,
								   &schema_name, &table_name, &column_name,
								   &datatype_name, &constraint_name);
		else if (PyErr_GivenExceptionMatches(val, PLy_exc_error))
			PLy_get_error_data(val, &sqlerrcode, &detail, &hint,
							   &schema_name, &table_name, &column_name,
							   &datatype_name, &constraint_name);
		else if (PyErr_GivenExceptionMatches(val, PLy_exc_fatal))
			elevel = FATAL;
	}

	/* this releases our refcount on tb! */
	PLy_traceback(exc, val, tb,
				  &xmsg, &tbmsg, &tb_depth);

	if (fmt)
	{
		initStringInfo(&emsg);
		for (;;)
		{
			va_list		ap;
			int			needed;

			va_start(ap, fmt);
			needed = appendStringInfoVA(&emsg, dgettext(TEXTDOMAIN, fmt), ap);
			va_end(ap);
			if (needed == 0)
				break;
			enlargeStringInfo(&emsg, needed);
		}
		primary = emsg.data;

		/* Since we have a format string, we cannot have a SPI detail. */
		Assert(detail == NULL);

		/* If there's an exception message, it goes in the detail. */
		if (xmsg)
			detail = xmsg;
	}
	else
	{
		if (xmsg)
			primary = xmsg;
	}

	PG_TRY();
	{
		ereport(elevel,
				(errcode(sqlerrcode ? sqlerrcode : ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			  errmsg_internal("%s", primary ? primary : "no exception data"),
				 (detail) ? errdetail_internal("%s", detail) : 0,
				 (tb_depth > 0 && tbmsg) ? errcontext("%s", tbmsg) : 0,
				 (hint) ? errhint("%s", hint) : 0,
				 (query) ? internalerrquery(query) : 0,
				 (position) ? internalerrposition(position) : 0,
				 (schema_name) ? err_generic_string(PG_DIAG_SCHEMA_NAME,
													schema_name) : 0,
				 (table_name) ? err_generic_string(PG_DIAG_TABLE_NAME,
												   table_name) : 0,
				 (column_name) ? err_generic_string(PG_DIAG_COLUMN_NAME,
													column_name) : 0,
				 (datatype_name) ? err_generic_string(PG_DIAG_DATATYPE_NAME,
													  datatype_name) : 0,
			  (constraint_name) ? err_generic_string(PG_DIAG_CONSTRAINT_NAME,
													 constraint_name) : 0));
	}
	PG_CATCH();
	{
		if (fmt)
			pfree(emsg.data);
		if (xmsg)
			pfree(xmsg);
		if (tbmsg)
			pfree(tbmsg);
		Py_XDECREF(exc);
		Py_XDECREF(val);

		PG_RE_THROW();
	}
	PG_END_TRY();

	if (fmt)
		pfree(emsg.data);
	if (xmsg)
		pfree(xmsg);
	if (tbmsg)
		pfree(tbmsg);
	Py_XDECREF(exc);
	Py_XDECREF(val);
}

/*
 * Extract a Python traceback from the given exception data.
 *
 * The exception error message is returned in xmsg, the traceback in
 * tbmsg (both as palloc'd strings) and the traceback depth in
 * tb_depth.
 *
 * We release refcounts on all the Python objects in the traceback stack,
 * but not on e or v.
 */
static void
PLy_traceback(PyObject *e, PyObject *v, PyObject *tb,
			  char **xmsg, char **tbmsg, int *tb_depth)
{
	PyObject   *e_type_o;
	PyObject   *e_module_o;
	char	   *e_type_s = NULL;
	char	   *e_module_s = NULL;
	PyObject   *vob = NULL;
	char	   *vstr;
	StringInfoData xstr;
	StringInfoData tbstr;

	/*
	 * if no exception, return nulls
	 */
	if (e == NULL)
	{
		*xmsg = NULL;
		*tbmsg = NULL;
		*tb_depth = 0;

		return;
	}

	/*
	 * Format the exception and its value and put it in xmsg.
	 */

	e_type_o = PyObject_GetAttrString(e, "__name__");
	e_module_o = PyObject_GetAttrString(e, "__module__");
	if (e_type_o)
		e_type_s = PyString_AsString(e_type_o);
	if (e_type_s)
		e_module_s = PyString_AsString(e_module_o);

	if (v && ((vob = PyObject_Str(v)) != NULL))
		vstr = PyString_AsString(vob);
	else
		vstr = "unknown";

	initStringInfo(&xstr);
	if (!e_type_s || !e_module_s)
	{
		if (PyString_Check(e))
			/* deprecated string exceptions */
			appendStringInfoString(&xstr, PyString_AsString(e));
		else
			/* shouldn't happen */
			appendStringInfoString(&xstr, "unrecognized exception");
	}
	/* mimics behavior of traceback.format_exception_only */
	else if (strcmp(e_module_s, "builtins") == 0
			 || strcmp(e_module_s, "__main__") == 0
			 || strcmp(e_module_s, "exceptions") == 0)
		appendStringInfo(&xstr, "%s", e_type_s);
	else
		appendStringInfo(&xstr, "%s.%s", e_module_s, e_type_s);
	appendStringInfo(&xstr, ": %s", vstr);

	*xmsg = xstr.data;

	/*
	 * Now format the traceback and put it in tbmsg.
	 */

	*tb_depth = 0;
	initStringInfo(&tbstr);
	/* Mimick Python traceback reporting as close as possible. */
	appendStringInfoString(&tbstr, "Traceback (most recent call last):");
	while (tb != NULL && tb != Py_None)
	{
		PyObject   *volatile tb_prev = NULL;
		PyObject   *volatile frame = NULL;
		PyObject   *volatile code = NULL;
		PyObject   *volatile name = NULL;
		PyObject   *volatile lineno = NULL;
		PyObject   *volatile filename = NULL;

		PG_TRY();
		{
			/*
			 * Ancient versions of Python (circa 2.3) contain a bug whereby
			 * the fetches below can fail if the error indicator is set.
			 */
			PyErr_Clear();

			lineno = PyObject_GetAttrString(tb, "tb_lineno");
			if (lineno == NULL)
				elog(ERROR, "could not get line number from Python traceback");

			frame = PyObject_GetAttrString(tb, "tb_frame");
			if (frame == NULL)
				elog(ERROR, "could not get frame from Python traceback");

			code = PyObject_GetAttrString(frame, "f_code");
			if (code == NULL)
				elog(ERROR, "could not get code object from Python frame");

			name = PyObject_GetAttrString(code, "co_name");
			if (name == NULL)
				elog(ERROR, "could not get function name from Python code object");

			filename = PyObject_GetAttrString(code, "co_filename");
			if (filename == NULL)
				elog(ERROR, "could not get file name from Python code object");
		}
		PG_CATCH();
		{
			Py_XDECREF(frame);
			Py_XDECREF(code);
			Py_XDECREF(name);
			Py_XDECREF(lineno);
			Py_XDECREF(filename);
			PG_RE_THROW();
		}
		PG_END_TRY();

		/* The first frame always points at <module>, skip it. */
		if (*tb_depth > 0)
		{
			PLyExecutionContext *exec_ctx = PLy_current_execution_context();
			char	   *proname;
			char	   *fname;
			char	   *line;
			char	   *plain_filename;
			long		plain_lineno;

			/*
			 * The second frame points at the internal function, but to mimick
			 * Python error reporting we want to say <module>.
			 */
			if (*tb_depth == 1)
				fname = "<module>";
			else
				fname = PyString_AsString(name);

			proname = PLy_procedure_name(exec_ctx->curr_proc);
			plain_filename = PyString_AsString(filename);
			plain_lineno = PyInt_AsLong(lineno);

			if (proname == NULL)
				appendStringInfo(
				&tbstr, "\n  PL/Python anonymous code block, line %ld, in %s",
								 plain_lineno - 1, fname);
			else
				appendStringInfo(
					&tbstr, "\n  PL/Python function \"%s\", line %ld, in %s",
								 proname, plain_lineno - 1, fname);

			/*
			 * function code object was compiled with "<string>" as the
			 * filename
			 */
			if (exec_ctx->curr_proc && plain_filename != NULL &&
				strcmp(plain_filename, "<string>") == 0)
			{
				/*
				 * If we know the current procedure, append the exact line
				 * from the source, again mimicking Python's traceback.py
				 * module behavior.  We could store the already line-split
				 * source to avoid splitting it every time, but producing a
				 * traceback is not the most important scenario to optimize
				 * for.  But we do not go as far as traceback.py in reading
				 * the source of imported modules.
				 */
				line = get_source_line(exec_ctx->curr_proc->src, plain_lineno);
				if (line)
				{
					appendStringInfo(&tbstr, "\n    %s", line);
					pfree(line);
				}
			}
		}

		Py_DECREF(frame);
		Py_DECREF(code);
		Py_DECREF(name);
		Py_DECREF(lineno);
		Py_DECREF(filename);

		/* Release the current frame and go to the next one. */
		tb_prev = tb;
		tb = PyObject_GetAttrString(tb, "tb_next");
		Assert(tb_prev != Py_None);
		Py_DECREF(tb_prev);
		if (tb == NULL)
			elog(ERROR, "could not traverse Python traceback");
		(*tb_depth)++;
	}

	/* Return the traceback. */
	*tbmsg = tbstr.data;

	Py_XDECREF(e_type_o);
	Py_XDECREF(e_module_o);
	Py_XDECREF(vob);
}

/*
 * Extract error code from SPIError's sqlstate attribute.
 */
static void
PLy_get_sqlerrcode(PyObject *exc, int *sqlerrcode)
{
	PyObject   *sqlstate;
	char	   *buffer;

	sqlstate = PyObject_GetAttrString(exc, "sqlstate");
	if (sqlstate == NULL)
		return;

	buffer = PyString_AsString(sqlstate);
	if (strlen(buffer) == 5 &&
		strspn(buffer, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") == 5)
	{
		*sqlerrcode = MAKE_SQLSTATE(buffer[0], buffer[1], buffer[2],
									buffer[3], buffer[4]);
	}

	Py_DECREF(sqlstate);
}

/*
 * Extract the error data from a SPIError
 */
static void
PLy_get_spi_error_data(PyObject *exc, int *sqlerrcode, char **detail,
					   char **hint, char **query, int *position,
					   char **schema_name, char **table_name,
					   char **column_name,
					   char **datatype_name, char **constraint_name)
{
	PyObject   *spidata;

	spidata = PyObject_GetAttrString(exc, "spidata");

	if (spidata != NULL)
	{
		PyArg_ParseTuple(spidata, "izzzizzzzz",
						 sqlerrcode, detail, hint, query, position,
						 schema_name, table_name, column_name,
						 datatype_name, constraint_name);
	}
	else
	{
		/*
		 * If there's no spidata, at least set the sqlerrcode. This can happen
		 * if someone explicitly raises a SPI exception from Python code.
		 */
		PLy_get_sqlerrcode(exc, sqlerrcode);
	}

	Py_XDECREF(spidata);
}

/*
 * Extract the error data from an Error.
 *
 * Note: position and query attributes are never set for Error so, unlike
 * PLy_get_spi_error_data, this function doesn't return them.
 */
static void
PLy_get_error_data(PyObject *exc, int *sqlerrcode, char **detail, char **hint,
				   char **schema_name, char **table_name, char **column_name,
				   char **datatype_name, char **constraint_name)
{
	PLy_get_sqlerrcode(exc, sqlerrcode);
	get_string_attr(exc, "detail", detail);
	get_string_attr(exc, "hint", hint);
	get_string_attr(exc, "schema_name", schema_name);
	get_string_attr(exc, "table_name", table_name);
	get_string_attr(exc, "column_name", column_name);
	get_string_attr(exc, "datatype_name", datatype_name);
	get_string_attr(exc, "constraint_name", constraint_name);
}

/*
 * Get the given source line as a palloc'd string
 */
static char *
get_source_line(const char *src, int lineno)
{
	const char *s = NULL;
	const char *next = src;
	int			current = 0;

	/* sanity check */
	if (lineno <= 0)
		return NULL;

	while (current < lineno)
	{
		s = next;
		next = strchr(s + 1, '\n');
		current++;
		if (next == NULL)
			break;
	}

	if (current != lineno)
		return NULL;

	while (*s && isspace((unsigned char) *s))
		s++;

	if (next == NULL)
		return pstrdup(s);

	/*
	 * Sanity check, next < s if the line was all-whitespace, which should
	 * never happen if Python reported a frame created on that line, but check
	 * anyway.
	 */
	if (next < s)
		return NULL;

	return pnstrdup(s, next - s);
}


/* call PyErr_SetString with a vprint interface and translation support */
void
PLy_exception_set(PyObject *exc, const char *fmt,...)
{
	char		buf[1024];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), dgettext(TEXTDOMAIN, fmt), ap);
	va_end(ap);

	PyErr_SetString(exc, buf);
}

/* same, with pluralized message */
void
PLy_exception_set_plural(PyObject *exc,
						 const char *fmt_singular, const char *fmt_plural,
						 unsigned long n,...)
{
	char		buf[1024];
	va_list		ap;

	va_start(ap, n);
	vsnprintf(buf, sizeof(buf),
			  dngettext(TEXTDOMAIN, fmt_singular, fmt_plural, n),
			  ap);
	va_end(ap);

	PyErr_SetString(exc, buf);
}

/* set attributes of the given exception to details from ErrorData */
void
PLy_exception_set_with_details(PyObject *excclass, ErrorData *edata)
{
	PyObject   *args = NULL;
	PyObject   *error = NULL;

	args = Py_BuildValue("(s)", edata->message);
	if (!args)
		goto failure;

	/* create a new exception with the error message as the parameter */
	error = PyObject_CallObject(excclass, args);
	if (!error)
		goto failure;

	if (!set_string_attr(error, "sqlstate",
						 unpack_sql_state(edata->sqlerrcode)))
		goto failure;

	if (!set_string_attr(error, "detail", edata->detail))
		goto failure;

	if (!set_string_attr(error, "hint", edata->hint))
		goto failure;

	if (!set_string_attr(error, "query", edata->internalquery))
		goto failure;

	if (!set_string_attr(error, "schema_name", edata->schema_name))
		goto failure;

	if (!set_string_attr(error, "table_name", edata->table_name))
		goto failure;

	if (!set_string_attr(error, "column_name", edata->column_name))
		goto failure;

	if (!set_string_attr(error, "datatype_name", edata->datatype_name))
		goto failure;

	if (!set_string_attr(error, "constraint_name", edata->constraint_name))
		goto failure;

	PyErr_SetObject(excclass, error);

	Py_DECREF(args);
	Py_DECREF(error);

	return;

failure:
	Py_XDECREF(args);
	Py_XDECREF(error);

	elog(ERROR, "could not convert error to Python exception");
}

/* get string value of an object attribute */
static void
get_string_attr(PyObject *obj, char *attrname, char **str)
{
	PyObject   *val;

	val = PyObject_GetAttrString(obj, attrname);
	if (val != NULL && val != Py_None)
	{
		*str = pstrdup(PyString_AsString(val));
	}
	Py_XDECREF(val);
}

/* set an object attribute to a string value, returns true when the set was
 * successful
 */
static bool
set_string_attr(PyObject *obj, char *attrname, char *str)
{
	int			result;
	PyObject   *val;

	if (str != NULL)
	{
		val = PyString_FromString(str);
		if (!val)
			return false;
	}
	else
	{
		val = Py_None;
		Py_INCREF(Py_None);
	}

	result = PyObject_SetAttrString(obj, attrname, val);
	Py_DECREF(val);

	return result != -1;
}
