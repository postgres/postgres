/* contrib/isn/isn--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION isn" to load this file. \quit

-- Example:
--   create table test ( id isbn );
--   insert into test values('978-0-393-04002-9');
--
--   select isbn('978-0-393-04002-9');
--   select isbn13('0-901690-54-6');
--

--
--	Input and output functions and data types:
--
---------------------------------------------------
CREATE FUNCTION ean13_in(cstring)
	RETURNS ean13
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION ean13_out(ean13)
	RETURNS cstring
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE TYPE ean13 (
	INPUT = ean13_in,
	OUTPUT = ean13_out,
	LIKE = pg_catalog.int8
);
COMMENT ON TYPE ean13
	IS 'International European Article Number (EAN13)';

CREATE FUNCTION isbn13_in(cstring)
	RETURNS isbn13
	AS 'MODULE_PATHNAME', 'isbn_in'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION ean13_out(isbn13)
	RETURNS cstring
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE TYPE isbn13 (
	INPUT = isbn13_in,
	OUTPUT = ean13_out,
	LIKE = pg_catalog.int8
);
COMMENT ON TYPE isbn13
	IS 'International Standard Book Number 13 (ISBN13)';

CREATE FUNCTION ismn13_in(cstring)
	RETURNS ismn13
	AS 'MODULE_PATHNAME', 'ismn_in'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION ean13_out(ismn13)
	RETURNS cstring
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE TYPE ismn13 (
	INPUT = ismn13_in,
	OUTPUT = ean13_out,
	LIKE = pg_catalog.int8
);
COMMENT ON TYPE ismn13
	IS 'International Standard Music Number 13 (ISMN13)';

CREATE FUNCTION issn13_in(cstring)
	RETURNS issn13
	AS 'MODULE_PATHNAME', 'issn_in'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION ean13_out(issn13)
	RETURNS cstring
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE TYPE issn13 (
	INPUT = issn13_in,
	OUTPUT = ean13_out,
	LIKE = pg_catalog.int8
);
COMMENT ON TYPE issn13
	IS 'International Standard Serial Number 13 (ISSN13)';

-- Short format:

CREATE FUNCTION isbn_in(cstring)
	RETURNS isbn
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION isn_out(isbn)
	RETURNS cstring
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE TYPE isbn (
	INPUT = isbn_in,
	OUTPUT = isn_out,
	LIKE = pg_catalog.int8
);
COMMENT ON TYPE isbn
	IS 'International Standard Book Number (ISBN)';

CREATE FUNCTION ismn_in(cstring)
	RETURNS ismn
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION isn_out(ismn)
	RETURNS cstring
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE TYPE ismn (
	INPUT = ismn_in,
	OUTPUT = isn_out,
	LIKE = pg_catalog.int8
);
COMMENT ON TYPE ismn
	IS 'International Standard Music Number (ISMN)';

CREATE FUNCTION issn_in(cstring)
	RETURNS issn
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION isn_out(issn)
	RETURNS cstring
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE TYPE issn (
	INPUT = issn_in,
	OUTPUT = isn_out,
	LIKE = pg_catalog.int8
);
COMMENT ON TYPE issn
	IS 'International Standard Serial Number (ISSN)';

CREATE FUNCTION upc_in(cstring)
	RETURNS upc
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION isn_out(upc)
	RETURNS cstring
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE TYPE upc (
	INPUT = upc_in,
	OUTPUT = isn_out,
	LIKE = pg_catalog.int8
);
COMMENT ON TYPE upc
	IS 'Universal Product Code (UPC)';

--
-- Operator functions:
--
---------------------------------------------------
-- EAN13:
CREATE FUNCTION isnlt(ean13, ean13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ean13, ean13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ean13, ean13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ean13, ean13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ean13, ean13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ean13, ean13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ean13, isbn13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ean13, isbn13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ean13, isbn13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ean13, isbn13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ean13, isbn13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ean13, isbn13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ean13, ismn13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ean13, ismn13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ean13, ismn13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ean13, ismn13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ean13, ismn13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ean13, ismn13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ean13, issn13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ean13, issn13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ean13, issn13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ean13, issn13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ean13, issn13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ean13, issn13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ean13, isbn)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ean13, isbn)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ean13, isbn)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ean13, isbn)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ean13, isbn)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ean13, isbn)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ean13, ismn)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ean13, ismn)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ean13, ismn)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ean13, ismn)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ean13, ismn)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ean13, ismn)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ean13, issn)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ean13, issn)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ean13, issn)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ean13, issn)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ean13, issn)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ean13, issn)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ean13, upc)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ean13, upc)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ean13, upc)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ean13, upc)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ean13, upc)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ean13, upc)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

---------------------------------------------------
-- ISBN13:
CREATE FUNCTION isnlt(isbn13, isbn13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(isbn13, isbn13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(isbn13, isbn13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(isbn13, isbn13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(isbn13, isbn13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(isbn13, isbn13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(isbn13, isbn)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(isbn13, isbn)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(isbn13, isbn)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(isbn13, isbn)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(isbn13, isbn)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(isbn13, isbn)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(isbn13, ean13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(isbn13, ean13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(isbn13, ean13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(isbn13, ean13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(isbn13, ean13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(isbn13, ean13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

---------------------------------------------------
-- ISBN:
CREATE FUNCTION isnlt(isbn, isbn)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(isbn, isbn)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(isbn, isbn)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(isbn, isbn)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(isbn, isbn)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(isbn, isbn)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(isbn, isbn13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(isbn, isbn13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(isbn, isbn13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(isbn, isbn13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(isbn, isbn13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(isbn, isbn13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(isbn, ean13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(isbn, ean13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(isbn, ean13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(isbn, ean13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(isbn, ean13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(isbn, ean13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

---------------------------------------------------
-- ISMN13:
CREATE FUNCTION isnlt(ismn13, ismn13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ismn13, ismn13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ismn13, ismn13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ismn13, ismn13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ismn13, ismn13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ismn13, ismn13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ismn13, ismn)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ismn13, ismn)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ismn13, ismn)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ismn13, ismn)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ismn13, ismn)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ismn13, ismn)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ismn13, ean13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ismn13, ean13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ismn13, ean13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ismn13, ean13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ismn13, ean13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ismn13, ean13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

---------------------------------------------------
-- ISMN:
CREATE FUNCTION isnlt(ismn, ismn)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ismn, ismn)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ismn, ismn)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ismn, ismn)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ismn, ismn)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ismn, ismn)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ismn, ismn13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ismn, ismn13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ismn, ismn13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ismn, ismn13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ismn, ismn13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ismn, ismn13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(ismn, ean13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(ismn, ean13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(ismn, ean13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(ismn, ean13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(ismn, ean13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(ismn, ean13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

---------------------------------------------------
-- ISSN13:
CREATE FUNCTION isnlt(issn13, issn13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(issn13, issn13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(issn13, issn13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(issn13, issn13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(issn13, issn13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(issn13, issn13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(issn13, issn)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(issn13, issn)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(issn13, issn)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(issn13, issn)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(issn13, issn)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(issn13, issn)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(issn13, ean13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(issn13, ean13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(issn13, ean13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(issn13, ean13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(issn13, ean13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(issn13, ean13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

---------------------------------------------------
-- ISSN:
CREATE FUNCTION isnlt(issn, issn)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(issn, issn)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(issn, issn)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(issn, issn)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(issn, issn)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(issn, issn)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(issn, issn13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(issn, issn13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(issn, issn13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(issn, issn13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(issn, issn13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(issn, issn13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(issn, ean13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(issn, ean13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(issn, ean13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(issn, ean13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(issn, ean13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(issn, ean13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

---------------------------------------------------
-- UPC:
CREATE FUNCTION isnlt(upc, upc)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(upc, upc)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(upc, upc)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(upc, upc)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(upc, upc)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(upc, upc)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE FUNCTION isnlt(upc, ean13)
	RETURNS boolean
	AS 'int8lt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnle(upc, ean13)
	RETURNS boolean
	AS 'int8le'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isneq(upc, ean13)
	RETURNS boolean
	AS 'int8eq'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnge(upc, ean13)
	RETURNS boolean
	AS 'int8ge'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isngt(upc, ean13)
	RETURNS boolean
	AS 'int8gt'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION isnne(upc, ean13)
	RETURNS boolean
	AS 'int8ne'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

--
-- Now the operators:
--

--
-- EAN13 operators:
--
---------------------------------------------------
CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ean13,
	RIGHTARG = ean13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ean13,
	RIGHTARG = ean13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ean13,
	RIGHTARG = ean13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ean13,
	RIGHTARG = ean13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ean13,
	RIGHTARG = ean13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ean13,
	RIGHTARG = ean13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ean13,
	RIGHTARG = isbn13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ean13,
	RIGHTARG = isbn13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ean13,
	RIGHTARG = isbn13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ean13,
	RIGHTARG = isbn13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ean13,
	RIGHTARG = isbn13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ean13,
	RIGHTARG = isbn13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = isbn13,
	RIGHTARG = ean13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = isbn13,
	RIGHTARG = ean13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = isbn13,
	RIGHTARG = ean13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = isbn13,
	RIGHTARG = ean13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = isbn13,
	RIGHTARG = ean13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = isbn13,
	RIGHTARG = ean13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ean13,
	RIGHTARG = ismn13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ean13,
	RIGHTARG = ismn13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ean13,
	RIGHTARG = ismn13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ean13,
	RIGHTARG = ismn13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ean13,
	RIGHTARG = ismn13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ean13,
	RIGHTARG = ismn13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ismn13,
	RIGHTARG = ean13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ismn13,
	RIGHTARG = ean13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ismn13,
	RIGHTARG = ean13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ismn13,
	RIGHTARG = ean13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ismn13,
	RIGHTARG = ean13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ismn13,
	RIGHTARG = ean13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ean13,
	RIGHTARG = issn13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ean13,
	RIGHTARG = issn13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ean13,
	RIGHTARG = issn13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ean13,
	RIGHTARG = issn13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ean13,
	RIGHTARG = issn13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ean13,
	RIGHTARG = issn13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ean13,
	RIGHTARG = isbn,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ean13,
	RIGHTARG = isbn,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ean13,
	RIGHTARG = isbn,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ean13,
	RIGHTARG = isbn,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ean13,
	RIGHTARG = isbn,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ean13,
	RIGHTARG = isbn,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ean13,
	RIGHTARG = ismn,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ean13,
	RIGHTARG = ismn,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ean13,
	RIGHTARG = ismn,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ean13,
	RIGHTARG = ismn,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ean13,
	RIGHTARG = ismn,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ean13,
	RIGHTARG = ismn,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ean13,
	RIGHTARG = issn,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ean13,
	RIGHTARG = issn,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ean13,
	RIGHTARG = issn,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ean13,
	RIGHTARG = issn,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ean13,
	RIGHTARG = issn,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ean13,
	RIGHTARG = issn,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ean13,
	RIGHTARG = upc,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ean13,
	RIGHTARG = upc,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ean13,
	RIGHTARG = upc,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ean13,
	RIGHTARG = upc,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ean13,
	RIGHTARG = upc,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel);
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ean13,
	RIGHTARG = upc,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

--
-- ISBN13 operators:
--
---------------------------------------------------
CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = isbn13,
	RIGHTARG = isbn13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = isbn13,
	RIGHTARG = isbn13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = isbn13,
	RIGHTARG = isbn13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = isbn13,
	RIGHTARG = isbn13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = isbn13,
	RIGHTARG = isbn13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = isbn13,
	RIGHTARG = isbn13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = isbn13,
	RIGHTARG = isbn,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = isbn13,
	RIGHTARG = isbn,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = isbn13,
	RIGHTARG = isbn,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = isbn13,
	RIGHTARG = isbn,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = isbn13,
	RIGHTARG = isbn,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = isbn13,
	RIGHTARG = isbn,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

--
-- ISBN operators:
--
---------------------------------------------------
CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = isbn,
	RIGHTARG = isbn,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = isbn,
	RIGHTARG = isbn,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = isbn,
	RIGHTARG = isbn,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = isbn,
	RIGHTARG = isbn,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = isbn,
	RIGHTARG = isbn,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = isbn,
	RIGHTARG = isbn,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = isbn,
	RIGHTARG = isbn13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = isbn,
	RIGHTARG = isbn13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = isbn,
	RIGHTARG = isbn13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = isbn,
	RIGHTARG = isbn13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = isbn,
	RIGHTARG = isbn13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = isbn,
	RIGHTARG = isbn13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = isbn,
	RIGHTARG = ean13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = isbn,
	RIGHTARG = ean13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = isbn,
	RIGHTARG = ean13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = isbn,
	RIGHTARG = ean13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = isbn,
	RIGHTARG = ean13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = isbn,
	RIGHTARG = ean13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

--
-- ISMN13 operators:
--
---------------------------------------------------
CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ismn13,
	RIGHTARG = ismn13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ismn13,
	RIGHTARG = ismn13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ismn13,
	RIGHTARG = ismn13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ismn13,
	RIGHTARG = ismn13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ismn13,
	RIGHTARG = ismn13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ismn13,
	RIGHTARG = ismn13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ismn13,
	RIGHTARG = ismn,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ismn13,
	RIGHTARG = ismn,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ismn13,
	RIGHTARG = ismn,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ismn13,
	RIGHTARG = ismn,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ismn13,
	RIGHTARG = ismn,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ismn13,
	RIGHTARG = ismn,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

--
-- ISMN operators:
--
---------------------------------------------------
CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ismn,
	RIGHTARG = ismn,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ismn,
	RIGHTARG = ismn,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ismn,
	RIGHTARG = ismn,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ismn,
	RIGHTARG = ismn,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ismn,
	RIGHTARG = ismn,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ismn,
	RIGHTARG = ismn,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ismn,
	RIGHTARG = ismn13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ismn,
	RIGHTARG = ismn13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ismn,
	RIGHTARG = ismn13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ismn,
	RIGHTARG = ismn13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ismn,
	RIGHTARG = ismn13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ismn,
	RIGHTARG = ismn13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = ismn,
	RIGHTARG = ean13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = ismn,
	RIGHTARG = ean13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = ismn,
	RIGHTARG = ean13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = ismn,
	RIGHTARG = ean13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = ismn,
	RIGHTARG = ean13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = ismn,
	RIGHTARG = ean13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

--
-- ISSN13 operators:
--
---------------------------------------------------
CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = issn13,
	RIGHTARG = issn13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = issn13,
	RIGHTARG = issn13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = issn13,
	RIGHTARG = issn13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = issn13,
	RIGHTARG = issn13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = issn13,
	RIGHTARG = issn13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = issn13,
	RIGHTARG = issn13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = issn13,
	RIGHTARG = issn,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = issn13,
	RIGHTARG = issn,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = issn13,
	RIGHTARG = issn,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = issn13,
	RIGHTARG = issn,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = issn13,
	RIGHTARG = issn,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = issn13,
	RIGHTARG = issn,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = issn13,
	RIGHTARG = ean13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = issn13,
	RIGHTARG = ean13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = issn13,
	RIGHTARG = ean13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = issn13,
	RIGHTARG = ean13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = issn13,
	RIGHTARG = ean13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = issn13,
	RIGHTARG = ean13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

--
-- ISSN operators:
--
---------------------------------------------------
CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = issn,
	RIGHTARG = issn,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = issn,
	RIGHTARG = issn,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = issn,
	RIGHTARG = issn,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = issn,
	RIGHTARG = issn,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = issn,
	RIGHTARG = issn,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = issn,
	RIGHTARG = issn,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = issn,
	RIGHTARG = issn13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = issn,
	RIGHTARG = issn13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = issn,
	RIGHTARG = issn13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = issn,
	RIGHTARG = issn13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = issn,
	RIGHTARG = issn13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = issn,
	RIGHTARG = issn13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = issn,
	RIGHTARG = ean13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = issn,
	RIGHTARG = ean13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = issn,
	RIGHTARG = ean13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = issn,
	RIGHTARG = ean13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = issn,
	RIGHTARG = ean13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = issn,
	RIGHTARG = ean13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

--
-- UPC operators:
--
---------------------------------------------------
CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = upc,
	RIGHTARG = upc,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = upc,
	RIGHTARG = upc,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = upc,
	RIGHTARG = upc,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = upc,
	RIGHTARG = upc,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = upc,
	RIGHTARG = upc,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = upc,
	RIGHTARG = upc,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

CREATE OPERATOR < (
	PROCEDURE = isnlt,
	LEFTARG = upc,
	RIGHTARG = ean13,
	COMMUTATOR = >,
	NEGATOR = >=,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR <= (
	PROCEDURE = isnle,
	LEFTARG = upc,
	RIGHTARG = ean13,
	COMMUTATOR = >=,
	NEGATOR = >,
	RESTRICT = scalarltsel,
	JOIN = scalarltjoinsel);
CREATE OPERATOR = (
	PROCEDURE = isneq,
	LEFTARG = upc,
	RIGHTARG = ean13,
	COMMUTATOR = =,
	NEGATOR = <>,
	RESTRICT = eqsel,
	JOIN = eqjoinsel,
	MERGES,
	HASHES);
CREATE OPERATOR >= (
	PROCEDURE = isnge,
	LEFTARG = upc,
	RIGHTARG = ean13,
	COMMUTATOR = <=,
	NEGATOR = <,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR > (
	PROCEDURE = isngt,
	LEFTARG = upc,
	RIGHTARG = ean13,
	COMMUTATOR = <,
	NEGATOR = <=,
	RESTRICT = scalargtsel,
	JOIN = scalargtjoinsel );
CREATE OPERATOR <> (
	PROCEDURE = isnne,
	LEFTARG = upc,
	RIGHTARG = ean13,
	COMMUTATOR = <>,
	NEGATOR = =,
	RESTRICT = neqsel,
	JOIN = neqjoinsel);

--
-- Operator families for the various operator classes:
--
---------------------------------------------------

CREATE OPERATOR FAMILY isn_ops USING btree;
CREATE OPERATOR FAMILY isn_ops USING hash;

--
-- Operator classes:
--
---------------------------------------------------
-- EAN13:
CREATE FUNCTION btean13cmp(ean13, ean13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS ean13_ops DEFAULT
	FOR TYPE ean13 USING btree FAMILY isn_ops AS
	OPERATOR 1  <,
	OPERATOR 2  <=,
	OPERATOR 3  =,
	OPERATOR 4  >=,
	OPERATOR 5  >,
	FUNCTION 1  btean13cmp(ean13, ean13);

CREATE FUNCTION hashean13(ean13)
	RETURNS int4
	AS 'hashint8'
	LANGUAGE 'internal' IMMUTABLE STRICT;

CREATE OPERATOR CLASS ean13_ops DEFAULT
	FOR TYPE ean13 USING hash FAMILY isn_ops AS
	OPERATOR 1  =,
	FUNCTION 1  hashean13(ean13);

-- EAN13 vs other types:
CREATE FUNCTION btean13cmp(ean13, isbn13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btean13cmp(ean13, ismn13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btean13cmp(ean13, issn13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btean13cmp(ean13, isbn)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btean13cmp(ean13, ismn)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btean13cmp(ean13, issn)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btean13cmp(ean13, upc)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

ALTER OPERATOR FAMILY isn_ops USING btree ADD
	OPERATOR 1  < (ean13, isbn13),
	OPERATOR 1  < (ean13, ismn13),
	OPERATOR 1  < (ean13, issn13),
	OPERATOR 1  < (ean13, isbn),
	OPERATOR 1  < (ean13, ismn),
	OPERATOR 1  < (ean13, issn),
	OPERATOR 1  < (ean13, upc),
	OPERATOR 2  <= (ean13, isbn13),
	OPERATOR 2  <= (ean13, ismn13),
	OPERATOR 2  <= (ean13, issn13),
	OPERATOR 2  <= (ean13, isbn),
	OPERATOR 2  <= (ean13, ismn),
	OPERATOR 2  <= (ean13, issn),
	OPERATOR 2  <= (ean13, upc),
	OPERATOR 3  = (ean13, isbn13),
	OPERATOR 3  = (ean13, ismn13),
	OPERATOR 3  = (ean13, issn13),
	OPERATOR 3  = (ean13, isbn),
	OPERATOR 3  = (ean13, ismn),
	OPERATOR 3  = (ean13, issn),
	OPERATOR 3  = (ean13, upc),
	OPERATOR 4  >= (ean13, isbn13),
	OPERATOR 4  >= (ean13, ismn13),
	OPERATOR 4  >= (ean13, issn13),
	OPERATOR 4  >= (ean13, isbn),
	OPERATOR 4  >= (ean13, ismn),
	OPERATOR 4  >= (ean13, issn),
	OPERATOR 4  >= (ean13, upc),
	OPERATOR 5  > (ean13, isbn13),
	OPERATOR 5  > (ean13, ismn13),
	OPERATOR 5  > (ean13, issn13),
	OPERATOR 5  > (ean13, isbn),
	OPERATOR 5  > (ean13, ismn),
	OPERATOR 5  > (ean13, issn),
	OPERATOR 5  > (ean13, upc),
	FUNCTION 1  btean13cmp(ean13, isbn13),
	FUNCTION 1  btean13cmp(ean13, ismn13),
	FUNCTION 1  btean13cmp(ean13, issn13),
	FUNCTION 1  btean13cmp(ean13, isbn),
	FUNCTION 1  btean13cmp(ean13, ismn),
	FUNCTION 1  btean13cmp(ean13, issn),
	FUNCTION 1  btean13cmp(ean13, upc);

ALTER OPERATOR FAMILY isn_ops USING hash ADD
	OPERATOR 1  = (ean13, isbn13),
	OPERATOR 1  = (ean13, ismn13),
	OPERATOR 1  = (ean13, issn13),
	OPERATOR 1  = (ean13, isbn),
	OPERATOR 1  = (ean13, ismn),
	OPERATOR 1  = (ean13, issn),
	OPERATOR 1  = (ean13, upc);

---------------------------------------------------
-- ISBN13:
CREATE FUNCTION btisbn13cmp(isbn13, isbn13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS isbn13_ops DEFAULT
	FOR TYPE isbn13 USING btree FAMILY isn_ops AS
	OPERATOR 1  <,
	OPERATOR 2  <=,
	OPERATOR 3  =,
	OPERATOR 4  >=,
	OPERATOR 5  >,
	FUNCTION 1  btisbn13cmp(isbn13, isbn13);

CREATE FUNCTION hashisbn13(isbn13)
	RETURNS int4
	AS 'hashint8'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS isbn13_ops DEFAULT
	FOR TYPE isbn13 USING hash FAMILY isn_ops AS
	OPERATOR 1  =,
	FUNCTION 1  hashisbn13(isbn13);

-- ISBN13 vs other types:
CREATE FUNCTION btisbn13cmp(isbn13, ean13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btisbn13cmp(isbn13, isbn)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

ALTER OPERATOR FAMILY isn_ops USING btree ADD
	OPERATOR 1  < (isbn13, ean13),
	OPERATOR 1  < (isbn13, isbn),
	OPERATOR 2  <= (isbn13, ean13),
	OPERATOR 2  <= (isbn13, isbn),
	OPERATOR 3  = (isbn13, ean13),
	OPERATOR 3  = (isbn13, isbn),
	OPERATOR 4  >= (isbn13, ean13),
	OPERATOR 4  >= (isbn13, isbn),
	OPERATOR 5  > (isbn13, ean13),
	OPERATOR 5  > (isbn13, isbn),
	FUNCTION 1  btisbn13cmp(isbn13, ean13),
	FUNCTION 1  btisbn13cmp(isbn13, isbn);

ALTER OPERATOR FAMILY isn_ops USING hash ADD
	OPERATOR 1  = (isbn13, ean13),
	OPERATOR 1  = (isbn13, isbn);

---------------------------------------------------
-- ISBN:
CREATE FUNCTION btisbncmp(isbn, isbn)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS isbn_ops DEFAULT
	FOR TYPE isbn USING btree FAMILY isn_ops AS
	OPERATOR 1  <,
	OPERATOR 2  <=,
	OPERATOR 3  =,
	OPERATOR 4  >=,
	OPERATOR 5  >,
	FUNCTION 1  btisbncmp(isbn, isbn);

CREATE FUNCTION hashisbn(isbn)
	RETURNS int4
	AS 'hashint8'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS isbn_ops DEFAULT
	FOR TYPE isbn USING hash FAMILY isn_ops AS
	OPERATOR 1  =,
	FUNCTION 1  hashisbn(isbn);

-- ISBN vs other types:
CREATE FUNCTION btisbncmp(isbn, ean13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btisbncmp(isbn, isbn13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

ALTER OPERATOR FAMILY isn_ops USING btree ADD
	OPERATOR 1  < (isbn, ean13),
	OPERATOR 1  < (isbn, isbn13),
	OPERATOR 2  <= (isbn, ean13),
	OPERATOR 2  <= (isbn, isbn13),
	OPERATOR 3  = (isbn, ean13),
	OPERATOR 3  = (isbn, isbn13),
	OPERATOR 4  >= (isbn, ean13),
	OPERATOR 4  >= (isbn, isbn13),
	OPERATOR 5  > (isbn, ean13),
	OPERATOR 5  > (isbn, isbn13),
	FUNCTION 1  btisbncmp(isbn, ean13),
	FUNCTION 1  btisbncmp(isbn, isbn13);

ALTER OPERATOR FAMILY isn_ops USING hash ADD
	OPERATOR 1  = (isbn, ean13),
	OPERATOR 1  = (isbn, isbn13);

---------------------------------------------------
-- ISMN13:
CREATE FUNCTION btismn13cmp(ismn13, ismn13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS ismn13_ops DEFAULT
	FOR TYPE ismn13 USING btree FAMILY isn_ops AS
	OPERATOR 1  <,
	OPERATOR 2  <=,
	OPERATOR 3  =,
	OPERATOR 4  >=,
	OPERATOR 5  >,
	FUNCTION 1  btismn13cmp(ismn13, ismn13);

CREATE FUNCTION hashismn13(ismn13)
	RETURNS int4
	AS 'hashint8'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS ismn13_ops DEFAULT
	FOR TYPE ismn13 USING hash FAMILY isn_ops AS
	OPERATOR 1  =,
	FUNCTION 1  hashismn13(ismn13);

-- ISMN13 vs other types:
CREATE FUNCTION btismn13cmp(ismn13, ean13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btismn13cmp(ismn13, ismn)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

ALTER OPERATOR FAMILY isn_ops USING btree ADD
	OPERATOR 1  < (ismn13, ean13),
	OPERATOR 1  < (ismn13, ismn),
	OPERATOR 2  <= (ismn13, ean13),
	OPERATOR 2  <= (ismn13, ismn),
	OPERATOR 3  = (ismn13, ean13),
	OPERATOR 3  = (ismn13, ismn),
	OPERATOR 4  >= (ismn13, ean13),
	OPERATOR 4  >= (ismn13, ismn),
	OPERATOR 5  > (ismn13, ean13),
	OPERATOR 5  > (ismn13, ismn),
	FUNCTION 1  btismn13cmp(ismn13, ean13),
	FUNCTION 1  btismn13cmp(ismn13, ismn);

ALTER OPERATOR FAMILY isn_ops USING hash ADD
	OPERATOR 1  = (ismn13, ean13),
	OPERATOR 1  = (ismn13, ismn);

---------------------------------------------------
-- ISMN:
CREATE FUNCTION btismncmp(ismn, ismn)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS ismn_ops DEFAULT
	FOR TYPE ismn USING btree FAMILY isn_ops AS
	OPERATOR 1  <,
	OPERATOR 2  <=,
	OPERATOR 3  =,
	OPERATOR 4  >=,
	OPERATOR 5  >,
	FUNCTION 1  btismncmp(ismn, ismn);

CREATE FUNCTION hashismn(ismn)
	RETURNS int4
	AS 'hashint8'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS ismn_ops DEFAULT
	FOR TYPE ismn USING hash FAMILY isn_ops AS
	OPERATOR 1  =,
	FUNCTION 1  hashismn(ismn);

-- ISMN vs other types:
CREATE FUNCTION btismncmp(ismn, ean13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btismncmp(ismn, ismn13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

ALTER OPERATOR FAMILY isn_ops USING btree ADD
	OPERATOR 1  < (ismn, ean13),
	OPERATOR 1  < (ismn, ismn13),
	OPERATOR 2  <= (ismn, ean13),
	OPERATOR 2  <= (ismn, ismn13),
	OPERATOR 3  = (ismn, ean13),
	OPERATOR 3  = (ismn, ismn13),
	OPERATOR 4  >= (ismn, ean13),
	OPERATOR 4  >= (ismn, ismn13),
	OPERATOR 5  > (ismn, ean13),
	OPERATOR 5  > (ismn, ismn13),
	FUNCTION 1  btismncmp(ismn, ean13),
	FUNCTION 1  btismncmp(ismn, ismn13);

ALTER OPERATOR FAMILY isn_ops USING hash ADD
	OPERATOR 1  = (ismn, ean13),
	OPERATOR 1  = (ismn, ismn13);

---------------------------------------------------
-- ISSN13:
CREATE FUNCTION btissn13cmp(issn13, issn13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS issn13_ops DEFAULT
	FOR TYPE issn13 USING btree FAMILY isn_ops AS
	OPERATOR 1  <,
	OPERATOR 2  <=,
	OPERATOR 3  =,
	OPERATOR 4  >=,
	OPERATOR 5  >,
	FUNCTION 1  btissn13cmp(issn13, issn13);

CREATE FUNCTION hashissn13(issn13)
	RETURNS int4
	AS 'hashint8'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS issn13_ops DEFAULT
	FOR TYPE issn13 USING hash FAMILY isn_ops AS
	OPERATOR 1  =,
	FUNCTION 1  hashissn13(issn13);

-- ISSN13 vs other types:
CREATE FUNCTION btissn13cmp(issn13, ean13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btissn13cmp(issn13, issn)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

ALTER OPERATOR FAMILY isn_ops USING btree ADD
	OPERATOR 1  < (issn13, ean13),
	OPERATOR 1  < (issn13, issn),
	OPERATOR 2  <= (issn13, ean13),
	OPERATOR 2  <= (issn13, issn),
	OPERATOR 3  = (issn13, ean13),
	OPERATOR 3  = (issn13, issn),
	OPERATOR 4  >= (issn13, ean13),
	OPERATOR 4  >= (issn13, issn),
	OPERATOR 5  > (issn13, ean13),
	OPERATOR 5  > (issn13, issn),
	FUNCTION 1  btissn13cmp(issn13, ean13),
	FUNCTION 1  btissn13cmp(issn13, issn);

ALTER OPERATOR FAMILY isn_ops USING hash ADD
	OPERATOR 1  = (issn13, ean13),
	OPERATOR 1  = (issn13, issn);

---------------------------------------------------
-- ISSN:
CREATE FUNCTION btissncmp(issn, issn)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS issn_ops DEFAULT
	FOR TYPE issn USING btree FAMILY isn_ops AS
	OPERATOR 1  <,
	OPERATOR 2  <=,
	OPERATOR 3  =,
	OPERATOR 4  >=,
	OPERATOR 5  >,
	FUNCTION 1  btissncmp(issn, issn);

CREATE FUNCTION hashissn(issn)
	RETURNS int4
	AS 'hashint8'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS issn_ops DEFAULT
	FOR TYPE issn USING hash FAMILY isn_ops AS
	OPERATOR 1  =,
	FUNCTION 1  hashissn(issn);

-- ISSN vs other types:
CREATE FUNCTION btissncmp(issn, ean13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;
CREATE FUNCTION btissncmp(issn, issn13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

ALTER OPERATOR FAMILY isn_ops USING btree ADD
	OPERATOR 1  < (issn, ean13),
	OPERATOR 1  < (issn, issn13),
	OPERATOR 2  <= (issn, ean13),
	OPERATOR 2  <= (issn, issn13),
	OPERATOR 3  = (issn, ean13),
	OPERATOR 3  = (issn, issn13),
	OPERATOR 4  >= (issn, ean13),
	OPERATOR 4  >= (issn, issn13),
	OPERATOR 5  > (issn, ean13),
	OPERATOR 5  > (issn, issn13),
	FUNCTION 1  btissncmp(issn, ean13),
	FUNCTION 1  btissncmp(issn, issn13);

ALTER OPERATOR FAMILY isn_ops USING hash ADD
	OPERATOR 1  = (issn, ean13),
	OPERATOR 1  = (issn, issn13);

---------------------------------------------------
-- UPC:
CREATE FUNCTION btupccmp(upc, upc)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS upc_ops DEFAULT
	FOR TYPE upc USING btree FAMILY isn_ops AS
	OPERATOR 1  <,
	OPERATOR 2  <=,
	OPERATOR 3  =,
	OPERATOR 4  >=,
	OPERATOR 5  >,
	FUNCTION 1  btupccmp(upc, upc);

CREATE FUNCTION hashupc(upc)
	RETURNS int4
	AS 'hashint8'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

CREATE OPERATOR CLASS upc_ops DEFAULT
	FOR TYPE upc USING hash FAMILY isn_ops AS
	OPERATOR 1  =,
	FUNCTION 1  hashupc(upc);

-- UPC vs other types:
CREATE FUNCTION btupccmp(upc, ean13)
	RETURNS int4
	AS 'btint8cmp'
	LANGUAGE 'internal'
	IMMUTABLE STRICT;

ALTER OPERATOR FAMILY isn_ops USING btree ADD
	OPERATOR 1  < (upc, ean13),
	OPERATOR 2  <= (upc, ean13),
	OPERATOR 3  = (upc, ean13),
	OPERATOR 4  >= (upc, ean13),
	OPERATOR 5  > (upc, ean13),
	FUNCTION 1  btupccmp(upc, ean13);

ALTER OPERATOR FAMILY isn_ops USING hash ADD
	OPERATOR 1  = (upc, ean13);

--
-- Type casts:
--
---------------------------------------------------
CREATE FUNCTION isbn13(ean13)
RETURNS isbn13
AS 'MODULE_PATHNAME', 'isbn_cast_from_ean13'
LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION ismn13(ean13)
RETURNS ismn13
AS 'MODULE_PATHNAME', 'ismn_cast_from_ean13'
LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION issn13(ean13)
RETURNS issn13
AS 'MODULE_PATHNAME', 'issn_cast_from_ean13'
LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION isbn(ean13)
RETURNS isbn
AS 'MODULE_PATHNAME', 'isbn_cast_from_ean13'
LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION ismn(ean13)
RETURNS ismn
AS 'MODULE_PATHNAME', 'ismn_cast_from_ean13'
LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION issn(ean13)
RETURNS issn
AS 'MODULE_PATHNAME', 'issn_cast_from_ean13'
LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION upc(ean13)
RETURNS upc
AS 'MODULE_PATHNAME', 'upc_cast_from_ean13'
LANGUAGE C IMMUTABLE STRICT;


CREATE CAST (ean13 AS isbn13) WITH FUNCTION isbn13(ean13);
CREATE CAST (ean13 AS isbn) WITH FUNCTION isbn(ean13);
CREATE CAST (ean13 AS ismn13) WITH FUNCTION ismn13(ean13);
CREATE CAST (ean13 AS ismn) WITH FUNCTION ismn(ean13);
CREATE CAST (ean13 AS issn13) WITH FUNCTION issn13(ean13);
CREATE CAST (ean13 AS issn) WITH FUNCTION issn(ean13);
CREATE CAST (ean13 AS upc) WITH FUNCTION upc(ean13);

CREATE CAST (isbn13 AS ean13) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (isbn AS ean13) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (ismn13 AS ean13) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (ismn AS ean13) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (issn13 AS ean13) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (issn AS ean13) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (upc AS ean13) WITHOUT FUNCTION AS ASSIGNMENT;

CREATE CAST (isbn AS isbn13) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (isbn13 AS isbn) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (ismn AS ismn13) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (ismn13 AS ismn) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (issn AS issn13) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (issn13 AS issn) WITHOUT FUNCTION AS ASSIGNMENT;

--
-- Validation stuff for lose types:
--
CREATE FUNCTION make_valid(ean13)
	RETURNS ean13
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION make_valid(isbn13)
	RETURNS isbn13
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION make_valid(ismn13)
	RETURNS ismn13
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION make_valid(issn13)
	RETURNS issn13
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION make_valid(isbn)
	RETURNS isbn
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION make_valid(ismn)
	RETURNS ismn
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION make_valid(issn)
	RETURNS issn
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION make_valid(upc)
	RETURNS upc
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;

CREATE FUNCTION is_valid(ean13)
	RETURNS boolean
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION is_valid(isbn13)
	RETURNS boolean
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION is_valid(ismn13)
	RETURNS boolean
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION is_valid(issn13)
	RETURNS boolean
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION is_valid(isbn)
	RETURNS boolean
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION is_valid(ismn)
	RETURNS boolean
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION is_valid(issn)
	RETURNS boolean
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;
CREATE FUNCTION is_valid(upc)
	RETURNS boolean
	AS 'MODULE_PATHNAME'
	LANGUAGE C
	IMMUTABLE STRICT;

--
-- isn_weak(boolean) - Sets the weak input mode.
-- This function is intended for testing use only!
--
CREATE FUNCTION isn_weak(boolean)
	RETURNS boolean
	AS 'MODULE_PATHNAME', 'accept_weak_input'
	LANGUAGE C
	IMMUTABLE STRICT;

--
-- isn_weak() - Gets the weak input mode status
--
CREATE FUNCTION isn_weak()
	RETURNS boolean
	AS 'MODULE_PATHNAME', 'weak_input_status'
	LANGUAGE C
	IMMUTABLE STRICT;
