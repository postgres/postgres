DO $$ plpy.notice("This is plpythonu.") $$ LANGUAGE plpythonu;

DO $$ plpy.notice("This is plpython2u.") $$ LANGUAGE plpython2u;

DO $$ nonsense $$ LANGUAGE plpythonu;
