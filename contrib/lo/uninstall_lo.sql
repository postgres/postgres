--
-- This removes the LO type
-- It's used just for development
--

-- Adjust this setting to control where the objects get created.
SET search_path = public;

-- drop the type and associated functions
DROP TYPE lo CASCADE;

-- the trigger function has no dependency on the type, so drop separately
DROP FUNCTION lo_manage();

-- the lo stuff is now removed from the system
