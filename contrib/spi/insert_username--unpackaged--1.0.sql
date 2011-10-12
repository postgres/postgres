/* contrib/spi/insert_username--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION insert_username" to load this file. \quit

ALTER EXTENSION insert_username ADD function insert_username();
