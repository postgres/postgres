/* contrib/dict_int/dict_int--unpackaged--1.0.sql */

ALTER EXTENSION dict_int ADD function dintdict_init(internal);
ALTER EXTENSION dict_int ADD function dintdict_lexize(internal,internal,internal,internal);
ALTER EXTENSION dict_int ADD text search template intdict_template;
ALTER EXTENSION dict_int ADD text search dictionary intdict;
