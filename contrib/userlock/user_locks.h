#ifndef USER_LOCKS_H
#define USER_LOCKS_H

int user_lock(unsigned int id1, unsigned int id2, LOCKT lockt);
int user_unlock(unsigned int id1, unsigned int id2, LOCKT lockt);
int user_write_lock(unsigned int id1, unsigned int id2);
int user_write_unlock(unsigned int id1, unsigned int id2);
int user_write_lock_oid(Oid oid);
int user_write_unlock_oid(Oid oid);
int user_unlock_all(void);

#endif
