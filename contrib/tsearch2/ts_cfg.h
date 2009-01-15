#ifndef __TS_CFG_H__
#define __TS_CFG_H__

#include "postgres.h"
#include "query.h"


typedef struct
{
	int			len;
	Datum	   *dict_id;
}	ListDictionary;

typedef struct
{
	Oid			id;
	Oid			prs_id;
	int			len;
	ListDictionary *map;
}	TSCfgInfo;

Oid			name2id_cfg(text *name);
TSCfgInfo  *findcfg(Oid id);
void		init_cfg(Oid id, TSCfgInfo * cfg);
void		reset_cfg(void);

typedef struct
{
	uint16		len;
	union
	{
		uint16		pos;
		uint16	   *apos;
	}			pos;
	char	   *word;
	uint32		alen;
}	TSWORD;

typedef struct
{
	TSWORD	   *words;
	int4		lenwords;
	int4		curwords;
	int4		pos;
}	PRSTEXT;

typedef struct
{
	uint32		selected:1,
				in:1,
				replace:1,
				repeated:1,
				skip:1,
				unused:3,
				type:8,
				len:16;
	char	   *word;
	ITEM	   *item;
}	HLWORD;

typedef struct
{
	HLWORD	   *words;
	int4		lenwords;
	int4		curwords;
	char	   *startsel;
	char	   *stopsel;
	int2		startsellen;
	int2		stopsellen;
}	HLPRSTEXT;

void		hlparsetext(TSCfgInfo * cfg, HLPRSTEXT * prs, QUERYTYPE * query, char *buf, int4 buflen);
text	   *genhl(HLPRSTEXT * prs);

void		parsetext_v2(TSCfgInfo * cfg, PRSTEXT * prs, char *buf, int4 buflen);
int			get_currcfg(void);

#endif
