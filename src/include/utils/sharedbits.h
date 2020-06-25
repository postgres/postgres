/*-------------------------------------------------------------------------
 *
 * sharedbits.h
 *	  Simple mechanism for sharing bits between backends.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/sharedbits.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHAREDBITS_H
#define SHAREDBITS_H

#include "storage/sharedfileset.h"

struct SharedBits;
typedef struct SharedBits SharedBits;

struct SharedBitsParticipant;
typedef struct SharedBitsParticipant SharedBitsParticipant;

struct SharedBitsAccessor;
typedef struct SharedBitsAccessor SharedBitsAccessor;

extern SharedBitsAccessor *sb_attach(SharedBits *sbits, int my_participant_number, SharedFileSet *fileset);
extern SharedBitsAccessor *sb_initialize(SharedBits *sbits, int participants, int my_participant_number, SharedFileSet *fileset, char *name);
extern void sb_initialize_accessor(SharedBitsAccessor *accessor, uint32 nbits);
extern size_t sb_estimate(int participants);

extern void sb_setbit(SharedBitsAccessor *accessor, uint64 bit);
extern bool sb_checkbit(SharedBitsAccessor *accessor, uint32 n);
extern BufFile *sb_combine(SharedBitsAccessor *accessor);

extern void sb_end_write(SharedBitsAccessor *sba);
extern void sb_end_read(SharedBitsAccessor *accessor);

#endif							/* SHAREDBITS_H */
