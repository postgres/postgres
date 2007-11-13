/* $PostgreSQL: pgsql/contrib/isn/uninstall_isn.sql,v 1.3 2007/11/13 04:24:28 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

--
--	Drop the actual types (in cascade):
--
DROP TYPE ean13 CASCADE;
DROP TYPE isbn13 CASCADE;
DROP TYPE ismn13 CASCADE;
DROP TYPE issn13 CASCADE;
DROP TYPE isbn CASCADE;
DROP TYPE ismn CASCADE;
DROP TYPE issn CASCADE;
DROP TYPE upc CASCADE;

DROP FUNCTION isn_weak();
DROP FUNCTION isn_weak(boolean);

