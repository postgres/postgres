/* 
 * ISpell interface
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "postgres.h"

#include "dict.h"
#include "common.h"

#define SYNBUFLEN	4096
typedef struct {
	char	*in;
	char	*out;
} Syn;

typedef struct {
	int	len;
	Syn	*syn;	
} DictSyn;

PG_FUNCTION_INFO_V1(syn_init);
Datum syn_init(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(syn_lexize);
Datum syn_lexize(PG_FUNCTION_ARGS);

static char *
findwrd(char *in, char **end) {
	char *start;

	*end=NULL;
	while(*in && isspace(*in))
		in++;

	if ( !in )
		return NULL;
	start=in;

	while(*in && !isspace(*in))
		in++;

	*end=in;
	return start;
}

static int
compareSyn(const void *a, const void *b) {
	return strcmp( ((Syn*)a)->in, ((Syn*)b)->in );
}


Datum 
syn_init(PG_FUNCTION_ARGS) {
	text       *in;
	DictSyn     *d;
	int cur=0;
	FILE 	*fin;
	char *filename;
	char buf[SYNBUFLEN];
	char *starti,*starto,*end=NULL;
	int slen;

	if ( PG_ARGISNULL(0) || PG_GETARG_POINTER(0)==NULL )
		elog(ERROR,"NULL config");

	in = PG_GETARG_TEXT_P(0);
	if ( VARSIZE(in) - VARHDRSZ == 0 )
		elog(ERROR,"VOID config");

	filename=text2char(in);
	PG_FREE_IF_COPY(in, 0);
	if ( (fin=fopen(filename,"r")) == NULL )
		elog(ERROR,"Can't open file '%s': %s", filename, strerror(errno));

	d = (DictSyn*)malloc( sizeof(DictSyn) );
	if ( !d ) {
		fclose(fin);
		elog(ERROR, "No memory");
	}
	memset(d,0,sizeof(DictSyn));

	while( fgets(buf,SYNBUFLEN,fin) ) {
		slen = strlen(buf)-1;
		buf[slen] = '\0';
		if ( *buf=='\0' ) continue;
		if (cur==d->len) {
			d->len = (d->len) ? 2*d->len : 16;
			d->syn=(Syn*)realloc( d->syn, sizeof(Syn)*d->len );
			if ( !d->syn ) {
				fclose(fin);
				elog(ERROR, "No memory");
			}
		}

		starti=findwrd(buf,&end);
		if ( !starti )
			continue;
		*end='\0';
		if ( end >= buf+slen )
			continue;

		starto= findwrd(end+1, &end);
		if ( !starto )
			continue;
		*end='\0';

		d->syn[cur].in=strdup(lowerstr(starti));
		d->syn[cur].out=strdup(lowerstr(starto));
		if ( !(d->syn[cur].in && d->syn[cur].out) ) {
			fclose(fin);
			elog(ERROR, "No memory");
		}

		cur++; 
	}
	
	fclose(fin);	
	
	d->len=cur;	
	if ( cur>1 )
		qsort(d->syn, d->len, sizeof(Syn), compareSyn);	

	pfree(filename);
        PG_RETURN_POINTER(d);
}

Datum
syn_lexize(PG_FUNCTION_ARGS) {
	DictSyn *d = (DictSyn*)PG_GETARG_POINTER(0);
	char       *in = (char*)PG_GETARG_POINTER(1);
	Syn	key,*found;
        char    **res=NULL;

	if ( !PG_GETARG_INT32(2) )
		PG_RETURN_POINTER(NULL);

	key.out=NULL;
	key.in=lowerstr(pnstrdup(in, PG_GETARG_INT32(2)));

	found=(Syn*)bsearch(&key, d->syn, d->len, sizeof(Syn), compareSyn);
	pfree(key.in);

	if ( !found ) 
		PG_RETURN_POINTER(NULL);

	res=palloc(sizeof(char*)*2);

	res[0]=pstrdup(found->out);
	res[1]=NULL;

        PG_RETURN_POINTER(res);
}

