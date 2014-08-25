/* contrib/spi/autoinc--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION autoinc FROM unpackaged" to load this file. \quit

ALTER EXTENSION autoinc ADD function autoinc();
