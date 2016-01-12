-- make sure the prerequisite libraries are loaded
LOAD 'plpython2';  -- change to plpython3 if that ever becomes the default
SELECT NULL::ltree;


CREATE FUNCTION ltree_to_plpython(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR ltree LANGUAGE plpythonu (
    FROM SQL WITH FUNCTION ltree_to_plpython(internal)
);
