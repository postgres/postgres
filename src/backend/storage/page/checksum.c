/*-------------------------------------------------------------------------
 *
 * checksum.c
 *	  Checksum implementation for data pages.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/page/checksum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/checksum.h"
/*
 * The actual code is in storage/checksum_impl.h.  This is done so that
 * external programs can incorporate the checksum code by #include'ing
 * that file from the exported Postgres headers.  (Compare our CRC code.)
 */
#include "storage/checksum_impl.h"
