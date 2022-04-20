/*-------------------------------------------------------------------------
 *
 * datapagemap.h
 *
 * Copyright (c) 2013-2022, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATAPAGEMAP_H
#define DATAPAGEMAP_H

#include "storage/block.h"
#include "storage/relfilenode.h"

struct datapagemap
{
	char	   *bitmap;
	int			bitmapsize;
};

typedef struct datapagemap datapagemap_t;
typedef struct datapagemap_iterator datapagemap_iterator_t;

extern void datapagemap_add(datapagemap_t *map, BlockNumber blkno);
extern datapagemap_iterator_t *datapagemap_iterate(datapagemap_t *map);
extern bool datapagemap_next(datapagemap_iterator_t *iter, BlockNumber *blkno);
extern void datapagemap_print(datapagemap_t *map);

#endif							/* DATAPAGEMAP_H */
