test_bloomfilter overview
=========================

test_bloomfilter is a test harness module for testing Bloom filter library set
membership operations.  It consists of a single SQL-callable function,
test_bloomfilter(), plus a regression test that calls test_bloomfilter().
Membership tests are performed against a dataset that the test harness module
generates.

The test_bloomfilter() function displays instrumentation at DEBUG1 elog level
(WARNING when the false positive rate exceeds a 1% threshold).  This can be
used to get a sense of the performance characteristics of the Postgres Bloom
filter implementation under varied conditions.

Bitset size
-----------

The main bloomfilter.c criteria for sizing its bitset is that the false
positive rate should not exceed 2% when sufficient bloom_work_mem is available
(and the caller-supplied estimate of the number of elements turns out to have
been accurate).  A 1% - 2% rate is currently assumed to be suitable for all
Bloom filter callers.

With an optimal K (number of hash functions), Bloom filters should only have a
1% false positive rate with just 9.6 bits of memory per element.  The Postgres
implementation's 2% worst case guarantee exists because there is a need for
some slop due to implementation inflexibility in bitset sizing.  Since the
bitset size is always actually kept to a power of two number of bits, callers
can have their bloom_work_mem argument truncated down by almost half.
In practice, callers that make a point of passing a bloom_work_mem that is an
exact power of two bitset size (such as test_bloomfilter.c) will actually get
the "9.6 bits per element" 1% false positive rate.

Testing strategy
----------------

Our approach to regression testing is to test that a Bloom filter has only a 1%
false positive rate for a single bitset size (2 ^ 23, or 1MB).  We test a
dataset with 838,861 elements, which works out at 10 bits of memory per
element.  We round up from 9.6 bits to 10 bits to make sure that we reliably
get under 1% for regression testing.  Note that a random seed is used in the
regression tests because the exact false positive rate is inconsistent across
platforms.  Inconsistent hash function behavior is something that the
regression tests need to be tolerant of anyway.

test_bloomfilter() SQL-callable function
========================================

The SQL-callable function test_bloomfilter() provides the following arguments:

* "power" is the power of two used to size the Bloom filter's bitset.

The minimum valid argument value is 23 (2^23 bits), or 1MB of memory.  The
maximum valid argument value is 32, or 512MB of memory.

* "nelements" is the number of elements to generate for testing purposes.

* "seed" is a seed value for hashing.

A value < 0 is interpreted as "use random seed".  Varying the seed value (or
specifying -1) should result in small variations in the total number of false
positives.

* "tests" is the number of tests to run.

This may be increased when it's useful to perform many tests in an interactive
session.  It only makes sense to perform multiple tests when a random seed is
used.
