--
-- This removes the type (and a test table)
-- It's used just for development
--

-- remove our test table
drop table a;

-- now drop the type and associated C functions
drop type lo CASCADE;

-- the trigger function has no dependency on the type, so drop separately
drop function lo_manage();

-- the lo stuff is now removed from the system
