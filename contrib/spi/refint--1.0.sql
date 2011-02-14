/* contrib/spi/refint--1.0.sql */

CREATE OR REPLACE FUNCTION check_primary_key()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE OR REPLACE FUNCTION check_foreign_key()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;
