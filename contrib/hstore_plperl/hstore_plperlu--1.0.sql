-- make sure the prerequisite libraries are loaded
LOAD 'plperl';
SELECT NULL::hstore;


CREATE FUNCTION hstore_to_plperlu(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'hstore_to_plperl';

CREATE FUNCTION plperlu_to_hstore(val internal) RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'plperl_to_hstore';

CREATE TRANSFORM FOR hstore LANGUAGE plperlu (
    FROM SQL WITH FUNCTION hstore_to_plperlu(internal),
    TO SQL WITH FUNCTION plperlu_to_hstore(internal)
);
