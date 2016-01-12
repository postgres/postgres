-- make sure the prerequisite libraries are loaded
LOAD 'plpython2';  -- change to plpython3 if that ever becomes the default
SELECT NULL::hstore;


CREATE FUNCTION hstore_to_plpython(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION plpython_to_hstore(val internal) RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR hstore LANGUAGE plpythonu (
    FROM SQL WITH FUNCTION hstore_to_plpython(internal),
    TO SQL WITH FUNCTION plpython_to_hstore(internal)
);

COMMENT ON TRANSFORM FOR hstore LANGUAGE plpythonu IS 'transform between hstore and Python dict';
