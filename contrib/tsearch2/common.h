#ifndef __TS_COMMON_H__
#define __TS_COMMON_H__
#include "postgres.h"
#include "fmgr.h"

#ifndef PG_NARGS
#define PG_NARGS() (fcinfo->nargs)
#endif

text	   *char2text(char *in);
text	   *charl2text(char *in, int len);
char	   *text2char(text *in);
char	   *pnstrdup(char *in, int len);
text	   *ptextdup(text *in);
text	   *mtextdup(text *in);

int			text_cmp(text *a, text *b);

#define NEXTVAL(x) ( (text*)( (char*)(x) + INTALIGN( VARSIZE(x) ) ) )
#define ARRNELEMS(x)  ArrayGetNItems( ARR_NDIM(x), ARR_DIMS(x))

void		ts_error(int state, const char *format,...);

#endif
