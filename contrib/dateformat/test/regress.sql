
---
--- Postgres DateStyle needs all tests which parsing 'now'::datetime string
---
SET DATESTYLE TO 'Postgres';


SELECT 'now'::datetime = 
	TO_CHAR('now'::datetime, 'Dy Mon DD HH24:MI:SS YYYY')::datetime 
as "Now vs. to_char";	

	
SELECT 'now'::datetime = 
	FROM_CHAR('now'::datetime, 'Dy Mon DD HH24:MI:SS YYYY')
as "Now vs. from_char";


SELECT	FROM_CHAR('now'::datetime, 'Dy Mon DD HH24:MI:SS YYYY') = 
	TO_CHAR('now'::datetime, 'Dy Mon DD HH24:MI:SS YYYY')::datetime 
as "From_char vs. To_char";	
	

SELECT 'now'::datetime = 	
	FROM_CHAR( 
		TO_CHAR('now'::datetime, '"Time: "HH24-MI-SS" Date: "Dy DD Mon YYYY'),
		'"Time: "HH24-MI-SS" Date: "Dy DD Mon YYYY'
	)
as "High from/to char test"; 	


SELECT TO_CHAR('now'::datetime, 'SSSS')::int = 
		TO_CHAR('now'::datetime, 'HH24')::int * 3600 + 
		TO_CHAR('now'::datetime, 'MI')::int * 60 + 
		TO_CHAR('now'::datetime, 'SS')::int
as "SSSS test";


SELECT TO_CHAR('now'::datetime, 'WW')::int =
		(TO_CHAR('now'::datetime, 'DDD')::int -
		TO_CHAR('now'::datetime, 'D')::int + 7) / 7 
as "Week test";	
	 

SELECT TO_CHAR('now'::datetime, 'Q')::int =
		TO_CHAR('now'::datetime, 'MM')::int / 3 + 1
as "Quartal test";


SELECT TO_CHAR('now'::datetime, 'DDD')::int =
	(TO_CHAR('now'::datetime, 'WW')::int * 7) -  
	(7 - TO_CHAR('now'::datetime, 'D')::int) +
	(7 - TO_CHAR(('01-Jan-'|| 
		TO_CHAR('now'::datetime,'YYYY'))::datetime,'D')::int)
	+1
as "Week and day test";



