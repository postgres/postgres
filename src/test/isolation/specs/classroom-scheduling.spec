# Classroom Scheduling test
#
# Ensure that the classroom is not scheduled more than once
# for any moment in time.
#
# Any overlap between the transactions must cause a serialization failure.

setup
{
 CREATE TABLE room_reservation (room_id text NOT NULL, start_time timestamp with time zone NOT NULL, end_time timestamp with time zone NOT NULL, description text NOT NULL, CONSTRAINT room_reservation_pkey PRIMARY KEY (room_id, start_time));
 INSERT INTO room_reservation VALUES ('101', TIMESTAMP WITH TIME ZONE '2010-04-01 10:00', TIMESTAMP WITH TIME ZONE '2010-04-01 11:00', 'Bob');
}

teardown
{
 DROP TABLE room_reservation;
}

session s1
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rx1	{ SELECT count(*) FROM room_reservation WHERE room_id = '101' AND start_time < TIMESTAMP WITH TIME ZONE '2010-04-01 14:00' AND end_time > TIMESTAMP WITH TIME ZONE '2010-04-01 13:00'; }
step wy1	{ INSERT INTO room_reservation VALUES ('101', TIMESTAMP WITH TIME ZONE '2010-04-01 13:00', TIMESTAMP WITH TIME ZONE '2010-04-01 14:00', 'Carol'); }
step c1		{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step ry2	{ SELECT count(*) FROM room_reservation WHERE room_id = '101' AND start_time < TIMESTAMP WITH TIME ZONE '2010-04-01 14:30' AND end_time > TIMESTAMP WITH TIME ZONE '2010-04-01 13:30'; }
step wx2	{ UPDATE room_reservation SET start_time = TIMESTAMP WITH TIME ZONE '2010-04-01 13:30', end_time = TIMESTAMP WITH TIME ZONE '2010-04-01 14:30' WHERE room_id = '101' AND start_time = TIMESTAMP WITH TIME ZONE '2010-04-01 10:00'; }
step c2		{ COMMIT; }
