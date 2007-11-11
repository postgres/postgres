-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

--
-- This removes the LO type
-- It's used just for development
--

-- drop the type and associated functions
DROP TYPE lo CASCADE;

-- the trigger function has no dependency on the type, so drop separately
DROP FUNCTION lo_manage();

-- the lo stuff is now removed from the system
