--
-- This removes the type (and a test table)
-- It's used just for development
--

-- Adjust this setting to control where the objects get created.
SET search_path = public;

-- remove our test table
DROP TABLE a;

-- now drop the type and associated C functions
DROP TYPE lo CASCADE;

-- the trigger function has no dependency on the type, so drop separately
DROP FUNCTION lo_manage();

-- the lo stuff is now removed from the system
