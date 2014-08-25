/* contrib/uuid-ossp/uuid-ossp--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use '''CREATE EXTENSION "uuid-ossp" FROM unpackaged''' to load this file. \quit

ALTER EXTENSION "uuid-ossp" ADD function uuid_nil();
ALTER EXTENSION "uuid-ossp" ADD function uuid_ns_dns();
ALTER EXTENSION "uuid-ossp" ADD function uuid_ns_url();
ALTER EXTENSION "uuid-ossp" ADD function uuid_ns_oid();
ALTER EXTENSION "uuid-ossp" ADD function uuid_ns_x500();
ALTER EXTENSION "uuid-ossp" ADD function uuid_generate_v1();
ALTER EXTENSION "uuid-ossp" ADD function uuid_generate_v1mc();
ALTER EXTENSION "uuid-ossp" ADD function uuid_generate_v3(namespace uuid, name text);
ALTER EXTENSION "uuid-ossp" ADD function uuid_generate_v4();
ALTER EXTENSION "uuid-ossp" ADD function uuid_generate_v5(namespace uuid, name text);
