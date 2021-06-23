setup
{
    CREATE TABLE trigtest(key text primary key, data text);

    CREATE FUNCTION noisy_oper(p_comment text, p_a anynonarray, p_op text, p_b anynonarray)
    RETURNS bool LANGUAGE plpgsql AS $body$
        DECLARE
            r bool;
        BEGIN
            EXECUTE format('SELECT $1 %s $2', p_op) INTO r USING p_a, p_b;
            RAISE NOTICE '%: % % % % %: %', p_comment, pg_typeof(p_a), p_a, p_op, pg_typeof(p_b), p_b, r;
        RETURN r;
    END;$body$;

    CREATE FUNCTION trig_report() RETURNS TRIGGER LANGUAGE plpgsql AS $body$
    DECLARE
	r_new text;
	r_old text;
	r_ret record;
    BEGIN
	-- In older releases it wasn't allowed to reference OLD/NEW
        -- when not applicable for TG_WHEN
	IF TG_OP = 'INSERT' THEN
	    r_old = NULL;
	    r_new = NEW;
	    r_ret = NEW;
	ELSIF TG_OP = 'DELETE' THEN
	    r_old = OLD;
	    r_new = NULL;
	    r_ret = OLD;
	ELSIF TG_OP = 'UPDATE' THEN
	    r_old = OLD;
	    r_new = NEW;
	    r_ret = NEW;
	END IF;

	IF TG_WHEN = 'AFTER' THEN
	   r_ret = NULL;
	END IF;

        RAISE NOTICE 'trigger: name %; when: %; lev: %s; op: %; old: % new: %',
            TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, r_old, r_new;

	RETURN r_ret;
    END;
    $body$;
}

teardown
{
     DROP TABLE trigtest;
     DROP FUNCTION noisy_oper(text, anynonarray, text, anynonarray);
     DROP FUNCTION trig_report();
}


session s0
step s0_rep { SELECT * FROM trigtest ORDER BY key, data }

session s1
#setup          {  }
step s1_b_rc     { BEGIN ISOLATION LEVEL READ COMMITTED; SELECT 1; }
step s1_b_rr     { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; }
step s1_c     { COMMIT; }
step s1_r     { ROLLBACK; }
step s1_trig_rep_b_i { CREATE TRIGGER rep_b_i BEFORE INSERT ON trigtest FOR EACH ROW EXECUTE PROCEDURE trig_report(); }
step s1_trig_rep_a_i { CREATE TRIGGER rep_a_i AFTER INSERT ON trigtest FOR EACH ROW EXECUTE PROCEDURE trig_report(); }
step s1_trig_rep_b_u { CREATE TRIGGER rep_b_u BEFORE UPDATE ON trigtest FOR EACH ROW EXECUTE PROCEDURE trig_report(); }
step s1_trig_rep_a_u { CREATE TRIGGER rep_a_u AFTER UPDATE ON trigtest FOR EACH ROW EXECUTE PROCEDURE trig_report(); }
step s1_trig_rep_b_d { CREATE TRIGGER rep_b_d BEFORE DELETE ON trigtest FOR EACH ROW EXECUTE PROCEDURE trig_report(); }
step s1_trig_rep_a_d { CREATE TRIGGER rep_a_d AFTER DELETE ON trigtest FOR EACH ROW EXECUTE PROCEDURE trig_report(); }
step s1_ins_a { INSERT INTO trigtest VALUES ('key-a', 'val-a-s1') RETURNING *; }
step s1_ins_b { INSERT INTO trigtest VALUES ('key-b', 'val-b-s1') RETURNING *; }
step s1_ins_c { INSERT INTO trigtest VALUES ('key-c', 'val-c-s1') RETURNING *; }
step s1_del_a {
    DELETE FROM trigtest
    WHERE
        noisy_oper('upd', key, '=', 'key-a') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *
}
step s1_del_b {
    DELETE FROM trigtest
    WHERE
        noisy_oper('upd', key, '=', 'key-b') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *
}
step s1_upd_a_data {
    UPDATE trigtest SET data = data || '-ups1'
    WHERE
        noisy_oper('upd', key, '=', 'key-a') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *;
}
step s1_upd_b_data {
    UPDATE trigtest SET data = data || '-ups1'
    WHERE
        noisy_oper('upd', key, '=', 'key-b') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *;
}
step s1_upd_a_tob {
    UPDATE trigtest SET key = 'key-b', data = data || '-tobs1'
    WHERE
        noisy_oper('upk', key, '=', 'key-a') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *;
}

session s2
#setup          {  }
step s2_b_rc     { BEGIN ISOLATION LEVEL READ COMMITTED; SELECT 1; }
step s2_b_rr     { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; }
step s2_c     { COMMIT; }
step s2_r     { ROLLBACK; }
step s2_ins_a { INSERT INTO trigtest VALUES ('key-a', 'val-a-s2') RETURNING *; }
step s2_del_a {
    DELETE FROM trigtest
    WHERE
        noisy_oper('upd', key, '=', 'key-a') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *
}
step s2_upd_a_data {
    UPDATE trigtest SET data = data || '-ups2'
    WHERE
        noisy_oper('upd', key, '=', 'key-a') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *;
}
step s2_upd_b_data {
    UPDATE trigtest SET data = data || '-ups2'
    WHERE
        noisy_oper('upd', key, '=', 'key-b') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *;
}
step s2_upd_all_data {
    UPDATE trigtest SET data = data || '-ups2'
    WHERE
        noisy_oper('upd', key, '<>', 'mismatch') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *;
}
step s2_upsert_a_data {
    INSERT INTO trigtest VALUES ('key-a', 'val-a-upss2')
    ON CONFLICT (key)
        DO UPDATE SET data = trigtest.data || '-upserts2'
        WHERE
            noisy_oper('upd', trigtest.key, '=', 'key-a') AND
            noisy_oper('upk', trigtest.data, '<>', 'mismatch')
    RETURNING *;
}

session s3
#setup          {  }
step s3_b_rc     { BEGIN ISOLATION LEVEL READ COMMITTED; SELECT 1; }
step s3_c     { COMMIT; }
step s3_r     { ROLLBACK; }
step s3_del_a {
    DELETE FROM trigtest
    WHERE
        noisy_oper('upd', key, '=', 'key-a') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *
}
step s3_upd_a_data {
    UPDATE trigtest SET data = data || '-ups3'
    WHERE
        noisy_oper('upd', key, '=', 'key-a') AND
        noisy_oper('upk', data, '<>', 'mismatch')
    RETURNING *;
}

### base case verifying that triggers see performed modifications
# s1 updates, s1 commits, s2 updates
permutation s1_trig_rep_b_u s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s1_c s2_upd_a_data s2_c
    s0_rep
# s1 updates, s1 rolls back, s2 updates
permutation s1_trig_rep_b_u s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s1_r s2_upd_a_data s2_c
    s0_rep
# s1 updates, s1 commits back, s2 deletes
permutation s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s1_c s2_del_a s2_c
    s0_rep
# s1 updates, s1 rolls back back, s2 deletes
permutation s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s1_r s2_del_a s2_c
    s0_rep

### Verify EPQ is performed if necessary, and skipped if transaction rolled back
# s1 updates, s2 updates, s1 commits, EPQ
permutation s1_trig_rep_b_u s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s2_upd_a_data s1_c s2_c
    s0_rep
# s1 updates, s2 updates, s1 rolls back, no EPQ
permutation s1_trig_rep_b_u s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s2_upd_a_data s1_r s2_c
    s0_rep
# s1 updates, s2 deletes, s1 commits, EPQ
permutation s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s2_upd_a_data s1_c s2_c
    s0_rep
# s1 updates, s2 deletes, s1 rolls back, no EPQ
permutation s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s2_upd_a_data s1_r s2_c
    s0_rep
# s1 deletes, s2 updates, s1 commits, EPQ
permutation s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_del_a s2_upd_a_data s1_c s2_c
    s0_rep
# s1 deletes, s2 updates, s1 rolls back, no EPQ
permutation s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_del_a s2_upd_a_data s1_r s2_c
    s0_rep
# s1 inserts, s2 inserts, s1 commits, s2 inserts, unique conflict
permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_a_i s1_trig_rep_a_d
    s1_b_rc s2_b_rc
    s1_ins_a s2_ins_a s1_c s2_c
    s0_rep
# s1 inserts, s2 inserts, s1 rolls back, s2 inserts, no unique conflict
permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_a_i s1_trig_rep_a_d
    s1_b_rc s2_b_rc
    s1_ins_a s2_ins_a s1_r s2_c
    s0_rep
# s1 updates, s2 upserts, s1 commits, EPQ
permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_i s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s2_upsert_a_data s1_c s2_c
    s0_rep
# s1 updates, s2 upserts, s1 rolls back, no EPQ
permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_i s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s2_upsert_a_data s1_c s2_c
    s0_rep
# s1 inserts, s2 upserts, s1 commits
permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_i s1_trig_rep_a_d s1_trig_rep_a_u
    s1_b_rc s2_b_rc
    s1_ins_a s2_upsert_a_data s1_c s2_c
    s0_rep
# s1 inserts, s2 upserts, s1 rolls back
permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_i s1_trig_rep_a_d s1_trig_rep_a_u
    s1_b_rc s2_b_rc
    s1_ins_a s2_upsert_a_data s1_r s2_c
    s0_rep
# s1 inserts, s2 upserts, s1 updates, s1 commits, EPQ
permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_i s1_trig_rep_a_d s1_trig_rep_a_u
    s1_b_rc s2_b_rc
    s1_ins_a s1_upd_a_data s2_upsert_a_data s1_c s2_c
    s0_rep
# s1 inserts, s2 upserts, s1 updates, s1 rolls back, no EPQ
permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_i s1_trig_rep_a_d s1_trig_rep_a_u
    s1_b_rc s2_b_rc
    s1_ins_a s1_upd_a_data s2_upsert_a_data s1_r s2_c
    s0_rep

### Verify EPQ is performed if necessary, and skipped if transaction rolled back,
### just without before triggers (for comparison, no additional row locks)
# s1 updates, s2 updates, s1 commits, EPQ
permutation s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s2_upd_a_data s1_c s2_c
    s0_rep
# s1 updates, s2 updates, s1 rolls back, no EPQ
permutation s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s2_upd_a_data s1_r s2_c
    s0_rep
# s1 updates, s2 deletes, s1 commits, EPQ
permutation s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s2_del_a s1_c s2_c
    s0_rep
# s1 updates, s2 deletes, s1 rolls back, no EPQ
permutation s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_upd_a_data s2_del_a s1_r s2_c
    s0_rep
# s1 deletes, s2 updates, s1 commits, EPQ
permutation s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_del_a s2_upd_a_data s1_c s2_c
    s0_rep
# s1 deletes, s2 updates, s1 rolls back, no EPQ
permutation s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_del_a s2_upd_a_data s1_r s2_c
    s0_rep
# s1 deletes, s2 deletes, s1 commits, EPQ
permutation s1_trig_rep_a_d
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_del_a s2_del_a s1_c s2_c
    s0_rep
# s1 deletes, s2 deletes, s1 rolls back, no EPQ
permutation s1_trig_rep_a_d
    s1_ins_a s1_ins_b s1_b_rc s2_b_rc
    s1_del_a s2_del_a s1_r s2_c
    s0_rep

### Verify that an update affecting a row that has been
### updated/deleted to not match the where clause anymore works
### correctly
# s1 updates to different key, s2 updates old key, s1 commits, EPQ failure should lead to no update
permutation s1_trig_rep_b_u s1_trig_rep_a_u
    s1_ins_a s1_ins_c s1_b_rc s2_b_rc
    s1_upd_a_tob s2_upd_a_data s1_c s2_c
    s0_rep
# s1 updates to different key, s2 updates old key, s1 rolls back, no EPQ failure
permutation s1_trig_rep_b_u s1_trig_rep_a_u
    s1_ins_a s1_ins_c s1_b_rc s2_b_rc
    s1_upd_a_tob s2_upd_a_data s1_r s2_c
    s0_rep
# s1 updates to different key, s2 updates new key, s1 commits, s2 will
# not see tuple with new key and not block
permutation s1_trig_rep_b_u s1_trig_rep_a_u
    s1_ins_a s1_ins_c s1_b_rc s2_b_rc
    s1_upd_a_tob s2_upd_b_data s1_c s2_c
    s0_rep
# s1 updates to different key, s2 updates all keys, s1 commits, s2,
# will not see tuple with old key, but block on old, and then follow
# the chain
permutation s1_trig_rep_b_u s1_trig_rep_a_u
    s1_ins_a s1_ins_c s1_b_rc s2_b_rc
    s1_upd_a_tob s2_upd_all_data s1_c s2_c
    s0_rep
# s1 deletes, s2 updates, s1 committs, EPQ failure should lead to no update
permutation s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_c s1_b_rc s2_b_rc
    s1_del_a s2_upd_a_data s1_c s2_c
    s0_rep
# s1 deletes, s2 updates, s1 rolls back, no EPQ failure
permutation s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_c s1_b_rc s2_b_rc
    s1_del_a s2_upd_a_data s1_r s2_c
    s0_rep
# s1 deletes, s2 deletes, s1 committs, EPQ failure should lead to no delete
permutation s1_trig_rep_b_d s1_trig_rep_a_d
    s1_ins_a s1_ins_c s1_b_rc s2_b_rc
    s1_del_a s2_del_a s1_c s2_c
    s0_rep
# s1 deletes, s2 deletes, s1 rolls back, no EPQ failure
permutation s1_trig_rep_b_d s1_trig_rep_a_d
    s1_ins_a s1_ins_c s1_b_rc s2_b_rc
    s1_del_a s2_del_a s1_r s2_c
    s0_rep

### Verify EPQ with more than two participants works
## XXX: Disable tests, there is some potential for instability here that's not yet fully understood
## s1 updates, s2 updates, s3 updates, s1 commits, s2 EPQ, s2 commits, s3 EPQ
#permutation s1_trig_rep_b_u s1_trig_rep_a_u
#    s1_ins_a s1_ins_b s1_b_rc s2_b_rc s3_b_rc
#    s1_upd_a_data s2_upd_a_data s3_upd_a_data s1_c s2_c s3_c
#    s0_rep
## s1 updates, s2 updates, s3 updates, s1 commits, s2 EPQ, s2 rolls back, s3 EPQ
#permutation s1_trig_rep_b_u s1_trig_rep_a_u
#    s1_ins_a s1_ins_b s1_b_rc s2_b_rc s3_b_rc
#    s1_upd_a_data s2_upd_a_data s3_upd_a_data s1_c s2_r s3_c
#    s0_rep
## s1 updates, s3 updates, s2 upserts, s1 updates, s1 commits, s3 EPQ, s3 deletes, s3 commits, s2 inserts without EPQ recheck
#permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_i s1_trig_rep_a_d s1_trig_rep_a_u
#    s1_ins_a s1_b_rc s2_b_rc s3_b_rc
#    s1_upd_a_data s3_upd_a_data s2_upsert_a_data s1_upd_a_data s1_c s3_del_a s3_c s2_c
#    s0_rep
## s1 updates, s3 updates, s2 upserts, s1 updates, s1 commits, s3 EPQ, s3 deletes, s3 rolls back, s2 EPQ
#permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_i s1_trig_rep_a_d s1_trig_rep_a_u
#    s1_ins_a s1_b_rc s2_b_rc s3_b_rc
#    s1_upd_a_data s3_upd_a_data s2_upsert_a_data s1_upd_a_data s1_c s3_del_a s3_r s2_c
#    s0_rep

### Document that EPQ doesn't "leap" onto a tuple that would match after blocking
# s1 inserts a, s1 updates b, s2 updates b, s1 deletes b, s1 updates a to b, s1 commits, s2 EPQ finds tuple deleted
permutation s1_trig_rep_b_i s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_i s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_b s1_b_rc s2_b_rc
    s1_ins_a s1_upd_b_data s2_upd_b_data s1_del_b s1_upd_a_tob s1_c s2_c
    s0_rep

### Triggers for EPQ detect serialization failures
# s1 updates, s2 updates, s1 commits, serialization failure
permutation s1_trig_rep_b_u s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rr s2_b_rr
    s1_upd_a_data s2_upd_a_data s1_c s2_c
    s0_rep
# s1 updates, s2 updates, s1 rolls back, s2 succeeds
permutation s1_trig_rep_b_u s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rr s2_b_rr
    s1_upd_a_data s2_upd_a_data s1_r s2_c
    s0_rep
# s1 deletes, s2 updates, s1 commits, serialization failure
permutation s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rr s2_b_rr
    s1_del_a s2_upd_a_data s1_c s2_c
    s0_rep
# s1 deletes, s2 updates, s1 rolls back, s2 succeeds
permutation s1_trig_rep_b_d s1_trig_rep_b_u s1_trig_rep_a_d s1_trig_rep_a_u
    s1_ins_a s1_ins_b s1_b_rr s2_b_rr
    s1_del_a s2_upd_a_data s1_r s2_c
    s0_rep
