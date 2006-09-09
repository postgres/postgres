--
--	Drop the actual types (in cascade):
--
---------------------------------------------------
SET search_path = public;

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

