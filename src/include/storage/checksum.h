/*-------------------------------------------------------------------------
 *
 * checksum.h
 *	  Checksum implementation for data pages.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/checksum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CHECKSUM_H
#define CHECKSUM_H

/*
 * Fowler-Noll-Vo 1a block checksum algorithm. The data argument should be
 * aligned on a 4-byte boundary.
 */
extern uint32 checksum_block(char *data, uint32 size);

#endif   /* CHECKSUM_H */
