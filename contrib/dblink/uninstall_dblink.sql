DROP FUNCTION dblink_current_query ();

DROP FUNCTION dblink_build_sql_update (text, int2vector, int4, _text, _text);

DROP FUNCTION dblink_build_sql_delete (text, int2vector, int4, _text);

DROP FUNCTION dblink_build_sql_insert (text, int2vector, int4, _text, _text);

DROP FUNCTION dblink_get_pkey (text);

DROP TYPE dblink_pkey_results;

DROP FUNCTION dblink_exec (text,bool);

DROP FUNCTION dblink_exec (text);

DROP FUNCTION dblink_exec (text,text,bool);

DROP FUNCTION dblink_exec (text,text);

DROP FUNCTION dblink (text,bool);

DROP FUNCTION dblink (text);

DROP FUNCTION dblink (text,text,bool);

DROP FUNCTION dblink (text,text);

DROP FUNCTION dblink_close (text,text,bool);

DROP FUNCTION dblink_close (text,text);

DROP FUNCTION dblink_close (text,bool);

DROP FUNCTION dblink_close (text);

DROP FUNCTION dblink_fetch (text,text,int,bool);

DROP FUNCTION dblink_fetch (text,text,int);

DROP FUNCTION dblink_fetch (text,int,bool);

DROP FUNCTION dblink_fetch (text,int);

DROP FUNCTION dblink_open (text,text,text,bool);

DROP FUNCTION dblink_open (text,text,text);

DROP FUNCTION dblink_open (text,text,bool);

DROP FUNCTION dblink_open (text,text);

DROP FUNCTION dblink_disconnect (text);

DROP FUNCTION dblink_disconnect ();

DROP FUNCTION dblink_connect (text, text);

DROP FUNCTION dblink_connect (text);

DROP FUNCTION dblink_cancel_query(text);

DROP FUNCTION dblink_error_message(text);

DROP FUNCTION dblink_get_connections();

DROP FUNCTION dblink_get_result(text);

DROP FUNCTION dblink_get_result(text, boolean);

DROP FUNCTION dblink_is_busy(text);

DROP FUNCTION dblink_send_query(text, text);
