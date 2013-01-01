/*-------------------------------------------------------------------------
 *
 * pg_crc.c
 *	  PostgreSQL CRC support
 *
 * This file simply #includes the CRC table definitions so that they are
 * available to programs linked with libpgport.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include "utils/pg_crc_tables.h"
