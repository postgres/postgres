#ifndef USER_LOCKS_H
#define USER_LOCKS_H

int			user_lock(unsigned int id1, unsigned int id2, LOCKMODE lockmode);
int			user_unlock(unsigned int id1, unsigned int id2, LOCKMODE lockmode);
int			user_write_lock(unsigned int id1, unsigned int id2);
int			user_write_unlock(unsigned int id1, unsigned int id2);
int			user_write_lock_oid(Oid oid);
int			user_write_unlock_oid(Oid oid);
int			user_unlock_all(void);

#endif

/*
 * Local Variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
