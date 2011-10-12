/* contrib/pgrowlocks/pgrowlocks--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgrowlocks" to load this file. \quit

ALTER EXTENSION pgrowlocks ADD function pgrowlocks(text);
