/* contrib/unaccent/uninstall_unaccent.sql */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION unaccent(regdictionary, text);
DROP FUNCTION unaccent(text);
DROP TEXT SEARCH DICTIONARY unaccent;
DROP TEXT SEARCH TEMPLATE unaccent;
DROP FUNCTION unaccent_init(internal);
DROP FUNCTION unaccent_lexize(internal,internal,internal,internal);
