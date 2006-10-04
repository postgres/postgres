#include "postgres.h"

#include "fmgr.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "utils/syscache.h"
#include "miscadmin.h"

#include "ts_cfg.h"
#include "dict.h"
#include "wparser.h"
#include "snmap.h"
#include "common.h"
#include "tsvector.h"



#include "common.h"
#include "wparser.h"
#include "ts_cfg.h"
#include "dict.h"


Oid			TSNSP_FunctionOid = InvalidOid;


text *
char2text(char *in)
{
	return charl2text(in, strlen(in));
}

text *
charl2text(char *in, int len)
{
	text	   *out = (text *) palloc(len + VARHDRSZ);

	memcpy(VARDATA(out), in, len);
	VARATT_SIZEP(out) = len + VARHDRSZ;
	return out;
}

char
		   *
text2char(text *in)
{
	char	   *out = palloc(VARSIZE(in));

	memcpy(out, VARDATA(in), VARSIZE(in) - VARHDRSZ);
	out[VARSIZE(in) - VARHDRSZ] = '\0';
	return out;
}

char
		   *
pnstrdup(char *in, int len)
{
	char	   *out = palloc(len + 1);

	memcpy(out, in, len);
	out[len] = '\0';
	return out;
}

text
		   *
ptextdup(text *in)
{
	text	   *out = (text *) palloc(VARSIZE(in));

	memcpy(out, in, VARSIZE(in));
	return out;
}

text
		   *
mtextdup(text *in)
{
	text	   *out = (text *) malloc(VARSIZE(in));

	if (!out)
		ts_error(ERROR, "No memory");
	memcpy(out, in, VARSIZE(in));
	return out;
}

void
ts_error(int state, const char *format,...)
{
	va_list		args;
	int			tlen = 128,
				len = 0;
	char	   *buf;

	reset_cfg();
	reset_dict();
	reset_prs();

	va_start(args, format);
	buf = palloc(tlen);
	len = vsnprintf(buf, tlen - 1, format, args);
	if (len >= tlen)
	{
		tlen = len + 1;
		buf = repalloc(buf, tlen);
		vsnprintf(buf, tlen - 1, format, args);
	}
	va_end(args);

	/* ?? internal error ?? */
	elog(state, "%s", buf);
	pfree(buf);
}

int
text_cmp(text *a, text *b)
{
	if (VARSIZE(a) == VARSIZE(b))
		return strncmp(VARDATA(a), VARDATA(b), VARSIZE(a) - VARHDRSZ);
	return (int) VARSIZE(a) - (int) VARSIZE(b);

}

char *
get_namespace(Oid funcoid)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	Form_pg_namespace nsp;
	Oid			nspoid;
	char	   *txt;

	tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(funcoid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for proc oid %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);
	nspoid = proc->pronamespace;
	ReleaseSysCache(tuple);

	tuple = SearchSysCache(NAMESPACEOID, ObjectIdGetDatum(nspoid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for namespace oid %u", nspoid);
	nsp = (Form_pg_namespace) GETSTRUCT(tuple);
	txt = pstrdup(NameStr((nsp->nspname)));
	ReleaseSysCache(tuple);

	return txt;
}

Oid
get_oidnamespace(Oid funcoid)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	Oid			nspoid;

	tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(funcoid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for proc oid %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);
	nspoid = proc->pronamespace;
	ReleaseSysCache(tuple);

	return nspoid;
}

 /* if path is relative, take it as relative to share dir */
char *
to_absfilename(char *filename)
{
	if (!is_absolute_path(filename))
	{
		char		sharepath[MAXPGPATH];
		char	   *absfn;

#ifdef	WIN32
		char		delim = '\\';
#else
		char		delim = '/';
#endif
		get_share_path(my_exec_path, sharepath);
		absfn = palloc(strlen(sharepath) + strlen(filename) + 2);
		sprintf(absfn, "%s%c%s", sharepath, delim, filename);
		filename = absfn;
	}

	return filename;
}
