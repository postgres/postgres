#ifndef __DICT_H__
#define __DICT_H__
#include "postgres.h"
#include "fmgr.h"

typedef struct
{
	int			len;
	char	  **stop;
	char	   *(*wordop) (char *);
}	StopList;

void		sortstoplist(StopList * s);
void		freestoplist(StopList * s);
void		readstoplist(text *in, StopList * s);
bool		searchstoplist(StopList * s, char *key);
char	   *lowerstr(char *str);

typedef struct
{
	Oid			dict_id;
	FmgrInfo	lexize_info;
	void	   *dictionary;
}	DictInfo;

void		init_dict(Oid id, DictInfo * dict);
DictInfo   *finddict(Oid id);
Oid			name2id_dict(text *name);
void		reset_dict(void);


/* simple parser of cfg string */
typedef struct
{
	char	   *key;
	char	   *value;
}	Map;

void		parse_cfgdict(text *in, Map ** m);

#endif
