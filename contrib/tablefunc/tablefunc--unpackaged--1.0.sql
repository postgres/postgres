/* contrib/tablefunc/tablefunc--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION tablefunc" to load this file. \quit

ALTER EXTENSION tablefunc ADD function normal_rand(integer,double precision,double precision);
ALTER EXTENSION tablefunc ADD function crosstab(text);
ALTER EXTENSION tablefunc ADD type tablefunc_crosstab_2;
ALTER EXTENSION tablefunc ADD type tablefunc_crosstab_3;
ALTER EXTENSION tablefunc ADD type tablefunc_crosstab_4;
ALTER EXTENSION tablefunc ADD function crosstab2(text);
ALTER EXTENSION tablefunc ADD function crosstab3(text);
ALTER EXTENSION tablefunc ADD function crosstab4(text);
ALTER EXTENSION tablefunc ADD function crosstab(text,integer);
ALTER EXTENSION tablefunc ADD function crosstab(text,text);
ALTER EXTENSION tablefunc ADD function connectby(text,text,text,text,integer,text);
ALTER EXTENSION tablefunc ADD function connectby(text,text,text,text,integer);
ALTER EXTENSION tablefunc ADD function connectby(text,text,text,text,text,integer,text);
ALTER EXTENSION tablefunc ADD function connectby(text,text,text,text,text,integer);
