#include "postgres.h"
#include "common.h"
#include "wparser.h"
#include "ts_cfg.h"
#include "dict.h"

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
