/*-------------------------------------------------------------------------
 *
 * pageinspect.h
 *	  Common functions for pageinspect.
 *
 * Copyright (c) 2017-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pageinspect/pageinspect.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PAGEINSPECT_H_
#define _PAGEINSPECT_H_

#include "storage/bufpage.h"

/* in rawpage.c */
extern Page get_page_from_raw(bytea *raw_page);

#endif							/* _PAGEINSPECT_H_ */
