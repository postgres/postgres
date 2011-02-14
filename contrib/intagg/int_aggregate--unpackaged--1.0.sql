/* contrib/intagg/int_aggregate--unpackaged--1.0.sql */

ALTER EXTENSION int_aggregate ADD function int_agg_state(internal,integer);
ALTER EXTENSION int_aggregate ADD function int_agg_final_array(internal);
ALTER EXTENSION int_aggregate ADD function int_array_aggregate(integer);
ALTER EXTENSION int_aggregate ADD function int_array_enum(integer[]);
