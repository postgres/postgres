/* 
 * ISpell interface
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "postgres.h"

#include "dict.h"
#include "common.h"
#include "ispell/spell.h"

typedef struct {
	StopList	stoplist;
	IspellDict	obj;
} DictISpell;

PG_FUNCTION_INFO_V1(spell_init);
Datum spell_init(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(spell_lexize);
Datum spell_lexize(PG_FUNCTION_ARGS);

static void
freeDictISpell(DictISpell  *d) {
	FreeIspell(&(d->obj));
	freestoplist(&(d->stoplist));
	free(d);
}

Datum 
spell_init(PG_FUNCTION_ARGS) {
	DictISpell	*d;
	Map *cfg, *pcfg;
	text *in;
	bool affloaded=false, dictloaded=false, stoploaded=false;

	if ( PG_ARGISNULL(0) || PG_GETARG_POINTER(0)==NULL )
		elog(ERROR,"ISpell confguration error");
 
	d = (DictISpell*)malloc( sizeof(DictISpell) );
	if ( !d )
		elog(ERROR, "No memory");
	memset(d,0,sizeof(DictISpell));
	d->stoplist.wordop=lowerstr;

	in = PG_GETARG_TEXT_P(0);
	parse_cfgdict(in,&cfg);
	PG_FREE_IF_COPY(in, 0);
	pcfg=cfg;
	while(pcfg->key) {
		if ( strcasecmp("DictFile", pcfg->key) == 0 ) {
			if ( dictloaded ) {
				freeDictISpell(d);
				elog(ERROR,"Dictionary already loaded");
			}
			if ( ImportDictionary(&(d->obj), pcfg->value) ) {
				freeDictISpell(d);
				elog(ERROR,"Can't load dictionary file (%s)", pcfg->value);
			}
			dictloaded=true;
		} else if ( strcasecmp("AffFile", pcfg->key) == 0 ) {
			if ( affloaded ) {
				freeDictISpell(d);
				elog(ERROR,"Affixes already loaded");
			}
			if ( ImportAffixes(&(d->obj), pcfg->value) ) {
				freeDictISpell(d);
				elog(ERROR,"Can't load affix file (%s)", pcfg->value);
			}
			affloaded=true;
		} else if ( strcasecmp("StopFile", pcfg->key) == 0 ) {
			text *tmp=char2text(pcfg->value);
			if ( stoploaded ) {
				freeDictISpell(d);
				elog(ERROR,"Stop words already loaded");
			}
			readstoplist(tmp, &(d->stoplist));
			sortstoplist(&(d->stoplist));
			pfree(tmp);
			stoploaded=true;
		} else {
			freeDictISpell(d);
			elog(ERROR,"Unknown option: %s => %s", pcfg->key, pcfg->value);
		}
		pfree(pcfg->key);
		pfree(pcfg->value);
		pcfg++;
	}
	pfree(cfg);

	if ( affloaded && dictloaded ) {
		SortDictionary(&(d->obj));
		SortAffixes(&(d->obj));
	} else if ( !affloaded ) {
		freeDictISpell(d);
		elog(ERROR,"No affixes");
	} else {
		freeDictISpell(d);
		elog(ERROR,"No dictionary");
	}

	PG_RETURN_POINTER(d);
}

Datum
spell_lexize(PG_FUNCTION_ARGS) {
	DictISpell *d = (DictISpell*)PG_GETARG_POINTER(0);
	char       *in = (char*)PG_GETARG_POINTER(1);
	char *txt;
	char	**res;
	char 	**ptr, **cptr;

	if ( !PG_GETARG_INT32(2) )
		PG_RETURN_POINTER(NULL);

	res=palloc(sizeof(char*)*2);
	txt = pnstrdup(in, PG_GETARG_INT32(2));
	res=NormalizeWord(&(d->obj), txt);
	pfree(txt);

	if ( res==NULL ) 
		PG_RETURN_POINTER(NULL);

	ptr=cptr=res;
	while(*ptr) {
		if ( searchstoplist(&(d->stoplist),*ptr) ) {
			pfree(*ptr);
			*ptr=NULL;
			ptr++;
		} else {
			*cptr=*ptr;
			cptr++; ptr++;
		}
	}
	*cptr=NULL;

	PG_RETURN_POINTER(res);
}

