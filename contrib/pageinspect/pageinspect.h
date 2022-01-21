/*-------------------------------------------------------------------------
 *
 * pageinspect.h
 *	  Common functions for pageinspect.
 *
 * Copyright (c) 2017-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pageinspect/pageinspect.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PAGEINSPECT_H_
#define _PAGEINSPECT_H_

#include "storage/bufpage.h"

/*
 * Extension version number, for supporting older extension versions' objects
 */
enum pageinspect_version
{
	PAGEINSPECT_V1_8,
	PAGEINSPECT_V1_9,
};

/* in rawpage.c */
extern Page get_page_from_raw(bytea *raw_page);

#endif							/* _PAGEINSPECT_H_ */
