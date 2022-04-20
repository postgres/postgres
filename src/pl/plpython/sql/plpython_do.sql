DO $$ plpy.notice("This is plpython3u.") $$ LANGUAGE plpython3u;

DO $$ raise Exception("error test") $$ LANGUAGE plpython3u;
