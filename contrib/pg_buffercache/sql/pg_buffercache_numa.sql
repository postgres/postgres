SELECT NOT(pg_numa_available()) AS skip_test \gset
\if :skip_test
\quit
\endif

-- We expect at least one entry for each buffer
select count(*) >= (select setting::bigint
                    from pg_settings
                    where name = 'shared_buffers')
from pg_buffercache_numa;

-- Check that the functions / views can't be accessed by default. To avoid
-- having to create a dedicated user, use the pg_database_owner pseudo-role.
SET ROLE pg_database_owner;
SELECT count(*) > 0 FROM pg_buffercache_numa;
RESET role;

-- Check that pg_monitor is allowed to query view / function
SET ROLE pg_monitor;
SELECT count(*) > 0 FROM pg_buffercache_numa;
RESET role;
