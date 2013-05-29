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


static void PLy_traceback(char **xmsg, char **tbmsg, int *tb_depth);
static void PLy_get_spi_error_data(PyObject *exc, int *sqlerrcode, char **detail,
					   char **hint, char **query, int *position);
static char *get_source_line(const char *src, int lineno);


/*
 * Emit a PG error or notice, together with any available info about
 * the current Python error, previously set by PLy_exception_set().
 * This should be used to propagate Python errors into PG.	If fmt is
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

	PyErr_Fetch(&exc, &val, &tb);
	if (exc != NULL)
	{
		if (PyErr_GivenExceptionMatches(val, PLy_exc_spi_error))
			PLy_get_spi_error_data(val, &sqlerrcode, &detail, &hint, &query, &position);
		else if (PyErr_GivenExceptionMatches(val, PLy_exc_fatal))
			elevel = FATAL;
	}
	PyErr_Restore(exc, val, tb);

	PLy_traceback(&xmsg, &tbmsg, &tb_depth);

	if (fmt)
	{
		initStringInfo(&emsg);
		for (;;)
		{
			va_list		ap;
			bool		success;

			va_start(ap, fmt);
			success = appendStringInfoVA(&emsg, dgettext(TEXTDOMAIN, fmt), ap);
			va_end(ap);
			if (success)
				break;
			enlargeStringInfo(&emsg, emsg.maxlen);
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
				(errcode(sqlerrcode ? sqlerrcode : ERRCODE_INTERNAL_ERROR),
			  errmsg_internal("%s", primary ? primary : "no exception data"),
				 (detail) ? errdetail_internal("%s", detail) : 0,
				 (tb_depth > 0 && tbmsg) ? errcontext("%s", tbmsg) : 0,
				 (hint) ? errhint("%s", hint) : 0,
				 (query) ? internalerrquery(query) : 0,
				 (position) ? internalerrposition(position) : 0));
	}
	PG_CATCH();
	{
		if (fmt)
			pfree(emsg.data);
		if (xmsg)
			pfree(xmsg);
		if (tbmsg)
			pfree(tbmsg);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (fmt)
		pfree(emsg.data);
	if (xmsg)
		pfree(xmsg);
	if (tbmsg)
		pfree(tbmsg);
}

/*
 * Extract a Python traceback from the current exception.
 *
 * The exception error message is returned in xmsg, the traceback in
 * tbmsg (both as palloc'd strings) and the traceback depth in
 * tb_depth.
 */
static void
PLy_traceback(char **xmsg, char **tbmsg, int *tb_depth)
{
	PyObject   *e,
			   *v,
			   *tb;
	PyObject   *e_type_o;
	PyObject   *e_module_o;
	char	   *e_type_s = NULL;
	char	   *e_module_s = NULL;
	PyObject   *vob = NULL;
	char	   *vstr;
	StringInfoData xstr;
	StringInfoData tbstr;

	/*
	 * get the current exception
	 */
	PyErr_Fetch(&e, &v, &tb);

	/*
	 * oops, no exception, return
	 */
	if (e == NULL)
	{
		*xmsg = NULL;
		*tbmsg = NULL;
		*tb_depth = 0;

		return;
	}

	PyErr_NormalizeException(&e, &v, &tb);

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
	Py_XDECREF(v);
	Py_DECREF(e);
}

/*
 * Extract error code from SPIError's sqlstate attribute.
 */
static void
PLy_get_spi_sqlerrcode(PyObject *exc, int *sqlerrcode)
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
PLy_get_spi_error_data(PyObject *exc, int *sqlerrcode, char **detail, char **hint, char **query, int *position)
{
	PyObject   *spidata = NULL;

	spidata = PyObject_GetAttrString(exc, "spidata");

	if (spidata != NULL)
	{
		PyArg_ParseTuple(spidata, "izzzi", sqlerrcode, detail, hint, query, position);
	}
	else
	{
		/*
		 * If there's no spidata, at least set the sqlerrcode. This can happen
		 * if someone explicitly raises a SPI exception from Python code.
		 */
		PLy_get_spi_sqlerrcode(exc, sqlerrcode);
	}

	PyErr_Clear();
	/* no elog here, we simply won't report the errhint, errposition etc */
	Py_XDECREF(spidata);
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
