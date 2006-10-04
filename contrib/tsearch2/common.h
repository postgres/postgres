#ifndef __TS_COMMON_H__
#define __TS_COMMON_H__

#include "postgres.h"
#include "fmgr.h"
#include "utils/array.h"

text	   *char2text(char *in);
text	   *charl2text(char *in, int len);
char	   *text2char(text *in);
char	   *pnstrdup(char *in, int len);
text	   *ptextdup(text *in);
text	   *mtextdup(text *in);

int			text_cmp(text *a, text *b);

char	   *to_absfilename(char *filename);

#define NEXTVAL(x) ( (text*)( (char*)(x) + INTALIGN( VARSIZE(x) ) ) )
#define ARRNELEMS(x)  ArrayGetNItems( ARR_NDIM(x), ARR_DIMS(x))

void		ts_error(int state, const char *format,...);

extern Oid	TSNSP_FunctionOid;	/* oid of called function, needed only for
								 * determ namespace, no more */
char	   *get_namespace(Oid funcoid);
Oid			get_oidnamespace(Oid funcoid);

#define SET_FUNCOID()	do {											\
		if ( fcinfo->flinfo && fcinfo->flinfo->fn_oid != InvalidOid )	\
				TSNSP_FunctionOid = fcinfo->flinfo->fn_oid;					  \
} while(0)

#endif
