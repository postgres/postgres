/* $PostgreSQL: pgsql/contrib/unaccent/uninstall_unaccent.sql,v 1.2 2009/11/14 18:24:32 tgl Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION unaccent(regdictionary, text);
DROP FUNCTION unaccent(text);
DROP TEXT SEARCH DICTIONARY unaccent;
DROP TEXT SEARCH TEMPLATE unaccent;
DROP FUNCTION unaccent_init(internal);
DROP FUNCTION unaccent_lexize(internal,internal,internal,internal);
