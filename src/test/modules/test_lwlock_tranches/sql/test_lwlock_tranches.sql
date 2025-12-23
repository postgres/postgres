CREATE EXTENSION test_lwlock_tranches;
SELECT test_lwlock_tranches();
SELECT test_lwlock_tranche_creation(NULL);
SELECT test_lwlock_tranche_creation(repeat('a', 64));
SELECT test_lwlock_tranche_creation('test');
SELECT test_lwlock_tranche_lookup('test_lwlock_tranches_startup');
SELECT test_lwlock_tranche_lookup('bogus');
SELECT test_lwlock_initialize(65535);
