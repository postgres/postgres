/* contrib/spi/moddatetime--1.0.sql */

CREATE FUNCTION moddatetime()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;
