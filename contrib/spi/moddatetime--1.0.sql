/* contrib/spi/moddatetime--1.0.sql */

CREATE OR REPLACE FUNCTION moddatetime()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;
