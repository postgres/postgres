/* contrib/uuid-ossp/uuid-ossp--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION uuid-ossp UPDATE TO '1.1'" to load this file. \quit

ALTER FUNCTION uuid_nil() PARALLEL SAFE;
ALTER FUNCTION uuid_ns_dns() PARALLEL SAFE;
ALTER FUNCTION uuid_ns_url() PARALLEL SAFE;
ALTER FUNCTION uuid_ns_oid() PARALLEL SAFE;
ALTER FUNCTION uuid_ns_x500() PARALLEL SAFE;
ALTER FUNCTION uuid_generate_v1() PARALLEL SAFE;
ALTER FUNCTION uuid_generate_v1mc() PARALLEL SAFE;
ALTER FUNCTION uuid_generate_v3(uuid, text) PARALLEL SAFE;
ALTER FUNCTION uuid_generate_v4() PARALLEL SAFE;
ALTER FUNCTION uuid_generate_v5(uuid, text) PARALLEL SAFE;
