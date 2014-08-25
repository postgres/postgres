/* contrib/spi/refint--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION refint FROM unpackaged" to load this file. \quit

ALTER EXTENSION refint ADD function check_primary_key();
ALTER EXTENSION refint ADD function check_foreign_key();
