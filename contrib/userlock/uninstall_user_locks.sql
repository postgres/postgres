SET search_path = public;

DROP FUNCTION user_unlock_all();

DROP FUNCTION user_write_unlock_oid(int4);

DROP FUNCTION user_write_lock_oid(int4);

DROP FUNCTION user_write_unlock_oid(oid);

DROP FUNCTION user_write_lock_oid(oid);

DROP FUNCTION user_write_unlock(int4,oid);

DROP FUNCTION user_write_lock(int4,oid);

DROP FUNCTION user_write_unlock(int4,int4);

DROP FUNCTION user_write_lock(int4,int4);

DROP FUNCTION user_unlock(int4,int4,int4);

DROP FUNCTION user_lock(int4,int4,int4);
