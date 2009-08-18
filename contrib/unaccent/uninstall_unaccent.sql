/* $PostgreSQL: pgsql/contrib/unaccent/uninstall_unaccent.sql,v 1.1 2009/08/18 10:34:39 teodor Exp $ */

DROP FUNCTION IF EXISTS unaccent(regdictionary, text) CASCADE;
DROP FUNCTION IF EXISTS unaccent(text) CASCADE;
DROP TEXT SEARCH DICTIONARY IF EXISTS unaccent CASCADE;
DROP TEXT SEARCH TEMPLATE IF EXISTS unaccent CASCADE;
DROP FUNCTION IF EXISTS unaccent_init(internal) CASCADE;
DROP FUNCTION IF EXISTS unaccent_lexize(internal,internal,internal,internal) CASCADE;

