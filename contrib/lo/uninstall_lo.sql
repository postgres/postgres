/* $PostgreSQL: pgsql/contrib/lo/uninstall_lo.sql,v 1.3 2007/11/13 04:24:28 momjian Exp $ */

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
