# Test updating a locked tuple.  When the lock doesn't conflict with the
# update, no blocking nor serializability problems should occur.

setup
{
   DROP TABLE IF EXISTS users, orders;
   CREATE TABLE users (id INTEGER PRIMARY KEY,
                       name varchar,
                       sometime timestamp);
   CREATE TABLE orders (id INTEGER PRIMARY KEY,
                        name varchar,
                        user_id INTEGER REFERENCES users (id));
   INSERT INTO users (id, name) VALUES (1, 'olivier');
   INSERT INTO orders (id, name) VALUES (1, 'order of olivier (1)');
}

teardown
{
   DROP TABLE users, orders;
}

session s1
step s1b   { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1u1  { UPDATE orders SET name = 'order of olivier (2)', user_id = 1 WHERE id = 1; }
step s1u2  { UPDATE orders SET name = 'order of olivier (3)', user_id = 1 WHERE id = 1; }
step s1c   { COMMIT; }

session s2
step s2b   { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2u   { UPDATE users SET sometime = '1830-10-04' WHERE id = 1; }
step s2c   { COMMIT; }

permutation s1b s2b s2u s2c s1u1 s1u2 s1c
permutation s1b s2b s2u s1u1 s2c s1u2 s1c
permutation s1b s2b s1u1 s2u s2c s1u2 s1c
permutation s1b s1u1 s2b s2u s2c s1u2 s1c
permutation s1b s1u1 s2b s1u2 s2u s2c s1c
permutation s1b s1u1 s1u2 s2b s2u s2c s1c
