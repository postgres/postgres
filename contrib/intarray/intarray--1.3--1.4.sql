/* contrib/intarray/intarray--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION intarray UPDATE TO '1.4'" to load this file. \quit

-- Remove <@ from the GiST opclasses, as it's not usefully indexable
-- due to mishandling of empty arrays.  (It's OK in GIN.)

ALTER OPERATOR FAMILY gist__int_ops USING gist
DROP OPERATOR 8 (_int4, _int4);

ALTER OPERATOR FAMILY gist__intbig_ops USING gist
DROP OPERATOR 8 (_int4, _int4);

-- Likewise for the old spelling ~.

ALTER OPERATOR FAMILY gist__int_ops USING gist
DROP OPERATOR 14 (_int4, _int4);

ALTER OPERATOR FAMILY gist__intbig_ops USING gist
DROP OPERATOR 14 (_int4, _int4);
