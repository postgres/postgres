#ifndef __WPARSER_H__
#define __WPARSER_H__
#include "postgres.h"
#include "fmgr.h"

typedef struct
{
	Oid			prs_id;
	FmgrInfo	start_info;
	FmgrInfo	getlexeme_info;
	FmgrInfo	end_info;
	FmgrInfo	headline_info;
	Oid			lextype;
	void	   *prs;
}	WParserInfo;

void		init_prs(Oid id, WParserInfo * prs);
WParserInfo *findprs(Oid id);
Oid			name2id_prs(text *name);
void		reset_prs(void);


typedef struct
{
	int			lexid;
	char	   *alias;
	char	   *descr;
}	LexDescr;

#endif
