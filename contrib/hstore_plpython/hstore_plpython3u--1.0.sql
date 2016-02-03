-- make sure the prerequisite libraries are loaded
LOAD 'plpython3';
SELECT NULL::hstore;


CREATE FUNCTION hstore_to_plpython3(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'hstore_to_plpython';

CREATE FUNCTION plpython3_to_hstore(val internal) RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'plpython_to_hstore';

CREATE TRANSFORM FOR hstore LANGUAGE plpython3u (
    FROM SQL WITH FUNCTION hstore_to_plpython3(internal),
    TO SQL WITH FUNCTION plpython3_to_hstore(internal)
);

COMMENT ON TRANSFORM FOR hstore LANGUAGE plpython3u IS 'transform between hstore and Python dict';
