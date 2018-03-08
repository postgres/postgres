/* src/test/modules/test_predtest/test_predtest--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_predtest" to load this file. \quit

CREATE FUNCTION test_predtest(query text,
  OUT strong_implied_by bool,
  OUT weak_implied_by bool,
  OUT strong_refuted_by bool,
  OUT weak_refuted_by bool,
  OUT s_i_holds bool,
  OUT w_i_holds bool,
  OUT s_r_holds bool,
  OUT w_r_holds bool)
STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
