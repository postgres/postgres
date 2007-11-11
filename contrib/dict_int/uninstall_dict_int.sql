-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP TEXT SEARCH DICTIONARY intdict;

DROP TEXT SEARCH TEMPLATE intdict_template;

DROP FUNCTION dintdict_init(internal);

DROP FUNCTION dintdict_lexize(internal,internal,internal,internal);
