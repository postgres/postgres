#ifndef __SNMAP_H__
#define __SNMAP_H__

#include "postgres.h"

typedef struct
{
	char	   *key;
	Oid			value;
	Oid			nsp;
}	SNMapEntry;

typedef struct
{
	int			len;
	int			reallen;
	SNMapEntry *list;
}	SNMap;

void		addSNMap(SNMap * map, char *key, Oid value);
void		addSNMap_t(SNMap * map, text *key, Oid value);
Oid			findSNMap(SNMap * map, char *key);
Oid			findSNMap_t(SNMap * map, text *key);
void		freeSNMap(SNMap * map);

#endif
