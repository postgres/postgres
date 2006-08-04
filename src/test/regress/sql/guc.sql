-- SET vacuum_cost_delay to some value
SET vacuum_cost_delay TO 400;
SHOW vacuum_cost_delay;

-- SET LOCAL has no effect outside of a transaction
SET LOCAL vacuum_cost_delay TO 500;
SHOW vacuum_cost_delay;

-- SET LOCAL within a transaction that commits
BEGIN;
SET LOCAL vacuum_cost_delay TO 500;
SHOW vacuum_cost_delay;
COMMIT;
SHOW vacuum_cost_delay;

-- SET should be reverted after ROLLBACK
BEGIN;
SET vacuum_cost_delay TO 600;
SHOW vacuum_cost_delay;
ROLLBACK;
SHOW vacuum_cost_delay;

-- Some tests with subtransactions
BEGIN;
SET vacuum_cost_delay TO 700;
SAVEPOINT first_sp;
SET vacuum_cost_delay TO 800;
ROLLBACK TO first_sp;
SHOW vacuum_cost_delay;
SAVEPOINT second_sp;
SET vacuum_cost_delay TO 900;
SAVEPOINT third_sp;
SET vacuum_cost_delay TO 1000;
SHOW vacuum_cost_delay;
ROLLBACK TO third_sp;
SHOW vacuum_cost_delay;
ROLLBACK TO second_sp;
SHOW vacuum_cost_delay;
ROLLBACK;

-- SET LOCAL with Savepoints
BEGIN;
SHOW vacuum_cost_delay;
SAVEPOINT sp;
SET LOCAL vacuum_cost_delay TO 300;
SHOW vacuum_cost_delay;
ROLLBACK TO sp;
SHOW vacuum_cost_delay;
ROLLBACK;

-- SET followed by SET LOCAL
BEGIN;
SET vacuum_cost_delay TO 400;
SET LOCAL vacuum_cost_delay TO 500;
SHOW vacuum_cost_delay;
COMMIT;
SHOW vacuum_cost_delay;

--
-- Test RESET.  We use datestyle because the reset value is forced by
-- pg_regress, so it doesn't depend on the installation's configuration.
--
SHOW datestyle;
SET datestyle = iso, ymd;
SHOW datestyle;
RESET datestyle;
SHOW datestyle;
