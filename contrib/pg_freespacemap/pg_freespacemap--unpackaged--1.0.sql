/* contrib/pg_freespacemap/pg_freespacemap--unpackaged--1.0.sql */

ALTER EXTENSION pg_freespacemap ADD function pg_freespace(regclass,bigint);
ALTER EXTENSION pg_freespacemap ADD function pg_freespace(regclass);
