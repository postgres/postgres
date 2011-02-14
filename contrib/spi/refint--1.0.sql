/* contrib/spi/refint--1.0.sql */

CREATE FUNCTION check_primary_key()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION check_foreign_key()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;
