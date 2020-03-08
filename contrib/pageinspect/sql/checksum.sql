--
-- Verify correct calculation of checksums
--
-- Postgres' checksum algorithm produces different answers on little-endian
-- and big-endian machines.  The results of this test also vary depending
-- on the configured block size.  This test has several different expected
-- results files to handle the following possibilities:
--
--	BLCKSZ	end	file
--	8K	LE	checksum.out
--	8K	BE	checksum_1.out
--
-- In future we might provide additional expected-results files for other
-- block sizes, but there seems little point as long as so many other
-- test scripts also show false failures for non-default block sizes.
--

-- This is to label the results files with blocksize:
SHOW block_size;

SHOW block_size \gset

-- Apply page_checksum() to some different data patterns and block numbers
SELECT blkno,
    page_checksum(decode(repeat('01', :block_size), 'hex'), blkno) AS checksum_01,
    page_checksum(decode(repeat('04', :block_size), 'hex'), blkno) AS checksum_04,
    page_checksum(decode(repeat('ff', :block_size), 'hex'), blkno) AS checksum_ff,
    page_checksum(decode(repeat('abcd', :block_size / 2), 'hex'), blkno) AS checksum_abcd,
    page_checksum(decode(repeat('e6d6', :block_size / 2), 'hex'), blkno) AS checksum_e6d6,
    page_checksum(decode(repeat('4a5e', :block_size / 2), 'hex'), blkno) AS checksum_4a5e
  FROM generate_series(0, 100, 50) AS a (blkno);
