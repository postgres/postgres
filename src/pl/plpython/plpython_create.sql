
CREATE FUNCTION plpython_call_handler() RETURNS opaque
    AS '/usr/local/lib/postgresql/langs/plpython.so'
    LANGUAGE 'c';

CREATE TRUSTED PROCEDURAL LANGUAGE 'plpython'
    HANDLER plpython_call_handler
    LANCOMPILER 'plpython';

