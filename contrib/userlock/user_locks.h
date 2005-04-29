#ifndef USER_LOCKS_H
#define USER_LOCKS_H

extern int	user_lock(uint32 id1, uint32 id2, LOCKMODE lockmode);
extern int	user_unlock(uint32 id1, uint32 id2, LOCKMODE lockmode);
extern int	user_write_lock(uint32 id1, uint32 id2);
extern int	user_write_unlock(uint32 id1, uint32 id2);
extern int	user_write_lock_oid(Oid oid);
extern int	user_write_unlock_oid(Oid oid);
extern int	user_unlock_all(void);

#endif
