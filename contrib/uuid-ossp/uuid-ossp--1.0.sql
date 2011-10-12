/* contrib/uuid-ossp/uuid-ossp--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION uuid-ossp" to load this file. \quit

CREATE FUNCTION uuid_nil()
RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_nil'
IMMUTABLE STRICT LANGUAGE C;

CREATE FUNCTION uuid_ns_dns()
RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_ns_dns'
IMMUTABLE STRICT LANGUAGE C;

CREATE FUNCTION uuid_ns_url()
RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_ns_url'
IMMUTABLE STRICT LANGUAGE C;

CREATE FUNCTION uuid_ns_oid()
RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_ns_oid'
IMMUTABLE STRICT LANGUAGE C;

CREATE FUNCTION uuid_ns_x500()
RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_ns_x500'
IMMUTABLE STRICT LANGUAGE C;

CREATE FUNCTION uuid_generate_v1()
RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_generate_v1'
VOLATILE STRICT LANGUAGE C;

CREATE FUNCTION uuid_generate_v1mc()
RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_generate_v1mc'
VOLATILE STRICT LANGUAGE C;

CREATE FUNCTION uuid_generate_v3(namespace uuid, name text)
RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_generate_v3'
IMMUTABLE STRICT LANGUAGE C;

CREATE FUNCTION uuid_generate_v4()
RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_generate_v4'
VOLATILE STRICT LANGUAGE C;

CREATE FUNCTION uuid_generate_v5(namespace uuid, name text)
RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_generate_v5'
IMMUTABLE STRICT LANGUAGE C;
