/*-------------------------------------------------------------------------
 *
 * checksum.c
 *	  Checksum implementation for data pages.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/page/checksum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "port/pg_cpu.h"
#include "storage/checksum.h"
/*
 * The actual code is in storage/checksum_impl.h.  This is done so that
 * external programs can incorporate the checksum code by #include'ing
 * that file from the exported Postgres headers.  (Compare our legacy
 * CRC code in pg_crc.h.)
 * The PG_CHECKSUM_INTERNAL symbol allows core to use hardware-specific
 * coding without affecting external programs.
 */
#define PG_CHECKSUM_INTERNAL
#include "storage/checksum_impl.h"	/* IWYU pragma: keep */


static uint32
pg_checksum_block_fallback(const PGChecksummablePage *page)
{
#include "storage/checksum_block_internal.h"
}

/*
 * AVX2-optimized block checksum algorithm.
 */
#ifdef USE_AVX2_WITH_RUNTIME_CHECK
pg_attribute_target("avx2")
static uint32
pg_checksum_block_avx2(const PGChecksummablePage *page)
{
#include "storage/checksum_block_internal.h"
}
#endif							/* USE_AVX2_WITH_RUNTIME_CHECK */

/*
 * Choose the best available checksum implementation.
 */
static uint32
pg_checksum_choose(const PGChecksummablePage *page)
{
	pg_checksum_block = pg_checksum_block_fallback;

#ifdef USE_AVX2_WITH_RUNTIME_CHECK
	if (x86_feature_available(PG_AVX2))
		pg_checksum_block = pg_checksum_block_avx2;
#endif

	return pg_checksum_block(page);
}

static uint32 (*pg_checksum_block) (const PGChecksummablePage *page) = pg_checksum_choose;
