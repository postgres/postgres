/*
 * simple but fast map from str to Oid
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"

#include "snmap.h"
#include "common.h"

static int
compareSNMapEntry(const void *a, const void *b)
{
	if (((SNMapEntry *) a)->nsp < ((SNMapEntry *) b)->nsp)
		return -1;
	else if (((SNMapEntry *) a)->nsp > ((SNMapEntry *) b)->nsp)
		return 1;
	else
		return strcmp(((SNMapEntry *) a)->key, ((SNMapEntry *) b)->key);
}

void
addSNMap(SNMap * map, char *key, Oid value)
{
	if (map->len >= map->reallen)
	{
		SNMapEntry *tmp;
		int			len = (map->reallen) ? 2 * map->reallen : 16;

		tmp = (SNMapEntry *) realloc(map->list, sizeof(SNMapEntry) * len);
		if (!tmp)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		map->reallen = len;
		map->list = tmp;
	}
	map->list[map->len].key = strdup(key);
	if (!map->list[map->len].key)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	map->list[map->len].nsp = get_oidnamespace(TSNSP_FunctionOid);
	map->list[map->len].value = value;
	map->len++;
	if (map->len > 1)
		qsort(map->list, map->len, sizeof(SNMapEntry), compareSNMapEntry);
}

void
addSNMap_t(SNMap * map, text *key, Oid value)
{
	char	   *k = text2char(key);

	addSNMap(map, k, value);
	pfree(k);
}

Oid
findSNMap(SNMap * map, char *key)
{
	SNMapEntry *ptr;
	SNMapEntry	ks;

	ks.key = key;
	ks.nsp = get_oidnamespace(TSNSP_FunctionOid);
	ks.value = 0;

	if (map->len == 0 || !map->list)
		return 0;
	ptr = (SNMapEntry *) bsearch(&ks, map->list, map->len, sizeof(SNMapEntry), compareSNMapEntry);
	return (ptr) ? ptr->value : 0;
}

Oid
findSNMap_t(SNMap * map, text *key)
{
	char	   *k = text2char(key);
	int			res;

	res = findSNMap(map, k);
	pfree(k);
	return res;
}

void
freeSNMap(SNMap * map)
{
	SNMapEntry *entry = map->list;

	if (map->list)
	{
		while (map->len)
		{
			if (entry->key)
				free(entry->key);
			entry++;
			map->len--;
		}
		free(map->list);
	}
	memset(map, 0, sizeof(SNMap));
}
