--
-- Cursor regression tests
--

BEGIN;

DECLARE foo1 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo2 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo3 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo4 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo5 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo6 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo7 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo8 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo9 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo10 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo11 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo12 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo13 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo14 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo15 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo16 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo17 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo18 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo19 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo20 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo21 SCROLL CURSOR FOR SELECT * FROM tenk1;

DECLARE foo22 SCROLL CURSOR FOR SELECT * FROM tenk2;

DECLARE foo23 SCROLL CURSOR FOR SELECT * FROM tenk1;

FETCH 1 in foo1;

FETCH 2 in foo2;

FETCH 3 in foo3;

FETCH 4 in foo4;

FETCH 5 in foo5;

FETCH 6 in foo6;

FETCH 7 in foo7;

FETCH 8 in foo8;

FETCH 9 in foo9;

FETCH 10 in foo10;

FETCH 11 in foo11;

FETCH 12 in foo12;

FETCH 13 in foo13;

FETCH 14 in foo14;

FETCH 15 in foo15;

FETCH 16 in foo16;

FETCH 17 in foo17;

FETCH 18 in foo18;

FETCH 19 in foo19;

FETCH 20 in foo20;

FETCH 21 in foo21;

FETCH 22 in foo22;

FETCH 23 in foo23;

FETCH backward 1 in foo23;

FETCH backward 2 in foo22;

FETCH backward 3 in foo21;

FETCH backward 4 in foo20;

FETCH backward 5 in foo19;

FETCH backward 6 in foo18;

FETCH backward 7 in foo17;

FETCH backward 8 in foo16;

FETCH backward 9 in foo15;

FETCH backward 10 in foo14;

FETCH backward 11 in foo13;

FETCH backward 12 in foo12;

FETCH backward 13 in foo11;

FETCH backward 14 in foo10;

FETCH backward 15 in foo9;

FETCH backward 16 in foo8;

FETCH backward 17 in foo7;

FETCH backward 18 in foo6;

FETCH backward 19 in foo5;

FETCH backward 20 in foo4;

FETCH backward 21 in foo3;

FETCH backward 22 in foo2;

FETCH backward 23 in foo1;

CLOSE foo1;

CLOSE foo2;

CLOSE foo3;

CLOSE foo4;

CLOSE foo5;

CLOSE foo6;

CLOSE foo7;

CLOSE foo8;

CLOSE foo9;

CLOSE foo10;

CLOSE foo11;

CLOSE foo12;

-- leave some cursors open, to test that auto-close works.

END;

--
-- NO SCROLL disallows backward fetching
--

BEGIN;

DECLARE foo24 NO SCROLL CURSOR FOR SELECT * FROM tenk1;

FETCH 1 FROM foo24;

FETCH BACKWARD 1 FROM foo24; -- should fail

END;

--
-- Cursors outside transaction blocks
--

BEGIN;

DECLARE foo25 SCROLL CURSOR WITH HOLD FOR SELECT * FROM tenk2;

FETCH FROM foo25;

FETCH FROM foo25;

COMMIT;

FETCH FROM foo25;

FETCH BACKWARD FROM foo25;

FETCH ABSOLUTE -1 FROM foo25;

CLOSE foo25;

--
-- ROLLBACK should close holdable cursors
--

BEGIN;

DECLARE foo26 CURSOR WITH HOLD FOR SELECT * FROM tenk1;

ROLLBACK;

-- should fail
FETCH FROM foo26;
