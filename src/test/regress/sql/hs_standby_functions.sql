--
-- Hot Standby tests
--
-- hs_standby_functions.sql
--

-- should fail
select txid_current();

select length(txid_current_snapshot()::text) >= 4;

select pg_start_backup('should fail');
select pg_switch_wal();
select pg_stop_backup();

-- should return no rows
select * from pg_prepared_xacts;

-- just the startup process
select locktype, virtualxid, virtualtransaction, mode, granted
from pg_locks where virtualxid = '1/1';

-- suicide is painless
select pg_cancel_backend(pg_backend_pid());
