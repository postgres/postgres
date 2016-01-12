-- make sure the prerequisite libraries are loaded
LOAD 'plperl';
SELECT NULL::hstore;


CREATE FUNCTION hstore_to_plperl(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION plperl_to_hstore(val internal) RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR hstore LANGUAGE plperl (
    FROM SQL WITH FUNCTION hstore_to_plperl(internal),
    TO SQL WITH FUNCTION plperl_to_hstore(internal)
);
