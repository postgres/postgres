/* $PostgreSQL: pgsql/contrib/isn/uninstall_isn.sql,v 1.4 2008/11/28 21:19:13 tgl Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

-- Drop the operator families (which don't depend on the types)
DROP OPERATOR FAMILY isn_ops USING btree CASCADE;
DROP OPERATOR FAMILY isn_ops USING hash CASCADE;

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

-- and clean up a couple miscellaneous functions
DROP FUNCTION isn_weak();
DROP FUNCTION isn_weak(boolean);
