/* contrib/intagg/intagg--unpackaged--1.0.sql */

ALTER EXTENSION intagg ADD function int_agg_state(internal,integer);
ALTER EXTENSION intagg ADD function int_agg_final_array(internal);
ALTER EXTENSION intagg ADD function int_array_aggregate(integer);
ALTER EXTENSION intagg ADD function int_array_enum(integer[]);
