/* contrib/spi/autoinc--1.0.sql */

CREATE FUNCTION autoinc()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;
