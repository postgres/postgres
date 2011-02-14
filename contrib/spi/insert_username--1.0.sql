/* contrib/spi/insert_username--1.0.sql */

CREATE OR REPLACE FUNCTION insert_username()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;
