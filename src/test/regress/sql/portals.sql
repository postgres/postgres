--
-- PORTALS
--

BEGIN;

DECLARE foo1 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo2 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo3 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo4 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo5 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo6 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo7 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo8 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo9 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo10 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo11 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo12 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo13 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo14 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo15 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo16 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo17 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo18 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo19 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo20 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo21 CURSOR FOR SELECT * FROM tenk1;

DECLARE foo22 CURSOR FOR SELECT * FROM tenk2;

DECLARE foo23 CURSOR FOR SELECT * FROM tenk1;

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

end;

