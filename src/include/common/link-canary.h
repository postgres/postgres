/*-------------------------------------------------------------------------
 *
 * link-canary.h
 *	  Detect whether src/common functions came from frontend or backend.
 *
 * Copyright (c) 2018-2025, PostgreSQL Global Development Group
 *
 * src/include/common/link-canary.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LINK_CANARY_H
#define LINK_CANARY_H

extern bool pg_link_canary_is_frontend(void);

#endif							/* LINK_CANARY_H */
