package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;

/*
 * $Id: TimestampTest.java,v 1.12 2003/09/22 04:55:00 barry Exp $
 *
 * Test get/setTimestamp for both timestamp with time zone and
 * timestamp without time zone datatypes
 *
 */
public class TimestampTest extends TestCase
{

	private Connection con;

	public TimestampTest(String name)
	{
		super(name);
	}

	protected void setUp() throws Exception
	{
		con = TestUtil.openDB();
		TestUtil.createTable(con, TSWTZ_TABLE, "ts timestamp with time zone");
		TestUtil.createTable(con, TSWOTZ_TABLE, "ts timestamp without time zone");
	}

	protected void tearDown() throws Exception
	{
		TestUtil.dropTable(con, TSWTZ_TABLE);
		TestUtil.dropTable(con, TSWOTZ_TABLE);
		TestUtil.closeDB(con);
	}

	/*
	 * Tests the timestamp methods in ResultSet on timestamp with time zone
			* we insert a known string value (don't use setTimestamp) then see that 
			* we get back the same value from getTimestamp
	 */
	public void testGetTimestampWTZ()
	{
		try
		{
			Statement stmt = con.createStatement();

			//Insert the three timestamp values in raw pg format
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS1WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS2WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS3WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS4WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS1WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS2WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS3WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS4WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS1WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS2WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS3WTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + TS4WTZ_PGFORMAT + "'")));												
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + new java.sql.Timestamp(tmpDate1.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + new java.sql.Timestamp(tmpDate2.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + new java.sql.Timestamp(tmpDate3.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + new java.sql.Timestamp(tmpDate4.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + new java.sql.Timestamp(tmpTime1.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + new java.sql.Timestamp(tmpTime2.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + new java.sql.Timestamp(tmpTime3.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWTZ_TABLE, "'" + new java.sql.Timestamp(tmpTime4.getTime()) + "'")));
			

			// Fall through helper
			timestampTestWTZ();

			assertEquals(20, stmt.executeUpdate("DELETE FROM " + TSWTZ_TABLE));

			stmt.close();
		}
		catch (Exception ex)
		{
			fail(ex.getMessage());
		}
	}

	/*
	 * Tests the timestamp methods in PreparedStatement on timestamp with time zone
			* we insert a value using setTimestamp then see that
			* we get back the same value from getTimestamp (which we know works as it was tested
			* independently of setTimestamp
	 */
	public void testSetTimestampWTZ()
	{
		try
		{
			Statement stmt = con.createStatement();
			PreparedStatement pstmt = con.prepareStatement(TestUtil.insertSQL(TSWTZ_TABLE, "?"));

			pstmt.setTimestamp(1, TS1WTZ);
			assertEquals(1, pstmt.executeUpdate());

			pstmt.setTimestamp(1, TS2WTZ);
			assertEquals(1, pstmt.executeUpdate());

			pstmt.setTimestamp(1, TS3WTZ);
			assertEquals(1, pstmt.executeUpdate());

			pstmt.setTimestamp(1, TS4WTZ);
			assertEquals(1, pstmt.executeUpdate());
			
			// With java.sql.Timestamp
			pstmt.setObject(1,TS1WTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,TS2WTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,TS3WTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());			
			pstmt.setObject(1,TS4WTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			
			// With Strings
			pstmt.setObject(1,TS1WTZ_PGFORMAT, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,TS2WTZ_PGFORMAT, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,TS3WTZ_PGFORMAT, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());			
			pstmt.setObject(1,TS4WTZ_PGFORMAT, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());

			// With java.sql.Date
			pstmt.setObject(1,tmpDate1, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,tmpDate2, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,tmpDate3, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());			
			pstmt.setObject(1,tmpDate4, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
		
			// With java.sql.Time	
			pstmt.setObject(1,tmpTime1, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,tmpTime2, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,tmpTime3, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());			
			pstmt.setObject(1,tmpTime4, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());															
			
			// Fall through helper
			timestampTestWTZ();

			assertEquals(20, stmt.executeUpdate("DELETE FROM " + TSWTZ_TABLE));

			pstmt.close();
			stmt.close();
		}
		catch (Exception ex)
		{
			fail(ex.getMessage());
		}
	}

	/*
	 * Tests the timestamp methods in ResultSet on timestamp without time zone
			* we insert a known string value (don't use setTimestamp) then see that 
			* we get back the same value from getTimestamp
	 */
	public void testGetTimestampWOTZ()
	{
		try
		{
			Statement stmt = con.createStatement();

			//Insert the three timestamp values in raw pg format
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS1WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS2WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS3WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS4WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS1WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS2WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS3WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS4WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS1WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS2WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS3WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + TS4WOTZ_PGFORMAT + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + new java.sql.Timestamp(tmpDate1WOTZ.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + new java.sql.Timestamp(tmpDate2WOTZ.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + new java.sql.Timestamp(tmpDate3WOTZ.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + new java.sql.Timestamp(tmpDate4WOTZ.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + new java.sql.Timestamp(tmpTime1WOTZ.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + new java.sql.Timestamp(tmpTime2WOTZ.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + new java.sql.Timestamp(tmpTime3WOTZ.getTime()) + "'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL(TSWOTZ_TABLE, "'" + new java.sql.Timestamp(tmpTime4WOTZ.getTime()) + "'")));												

			// Fall through helper
			timestampTestWOTZ();

			assertEquals(20, stmt.executeUpdate("DELETE FROM " + TSWOTZ_TABLE));

			stmt.close();
		}
		catch (Exception ex)
		{
			fail(ex.getMessage());
		}
	}


	/*
	 * Tests the timestamp methods in PreparedStatement on timestamp without time zone
			* we insert a value using setTimestamp then see that
			* we get back the same value from getTimestamp (which we know works as it was tested
			* independently of setTimestamp
	 */
	public void testSetTimestampWOTZ()
	{
		try
		{
			Statement stmt = con.createStatement();
			PreparedStatement pstmt = con.prepareStatement(TestUtil.insertSQL(TSWOTZ_TABLE, "?"));

			pstmt.setTimestamp(1, TS1WOTZ);
			assertEquals(1, pstmt.executeUpdate());

			pstmt.setTimestamp(1, TS2WOTZ);
			assertEquals(1, pstmt.executeUpdate());

			pstmt.setTimestamp(1, TS3WOTZ);
			assertEquals(1, pstmt.executeUpdate());

			pstmt.setTimestamp(1, TS4WOTZ);
			assertEquals(1, pstmt.executeUpdate());


			// With java.sql.Timestamp
			pstmt.setObject(1,TS1WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,TS2WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,TS3WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());			
			pstmt.setObject(1,TS4WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			
			// With Strings
			pstmt.setObject(1,TS1WOTZ_PGFORMAT, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,TS2WOTZ_PGFORMAT, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,TS3WOTZ_PGFORMAT, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());			
			pstmt.setObject(1,TS4WOTZ_PGFORMAT, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());

			// With java.sql.Date
			pstmt.setObject(1,tmpDate1WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,tmpDate2WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,tmpDate3WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());			
			pstmt.setObject(1,tmpDate4WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
		
			// With java.sql.Time	
			pstmt.setObject(1,tmpTime1WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,tmpTime2WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());
			pstmt.setObject(1,tmpTime3WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());			
			pstmt.setObject(1,tmpTime4WOTZ, java.sql.Types.TIMESTAMP);
			assertEquals(1, pstmt.executeUpdate());															

			// Fall through helper
			timestampTestWOTZ();

			assertEquals(20, stmt.executeUpdate("DELETE FROM " + TSWOTZ_TABLE));

			pstmt.close();
			stmt.close();
		}
		catch (Exception ex)
		{
			fail(ex.getMessage());
		}
	}

	/*
	 * Helper for the TimestampTests. It tests what should be in the db
	 */
	private void timestampTestWTZ() throws SQLException
	{
		Statement stmt = con.createStatement();
		ResultSet rs;
		java.sql.Timestamp t;

		rs = stmt.executeQuery("select ts from " + TSWTZ_TABLE); //removed the order by ts
		assertNotNull(rs);

		for (int i=0; i<3; i++)
		{
			assertTrue(rs.next());
			t = rs.getTimestamp(1);
			assertNotNull(t);
			assertTrue(t.equals(TS1WTZ));
	
			assertTrue(rs.next());
			t = rs.getTimestamp(1);
			assertNotNull(t);
			assertTrue(t.equals(TS2WTZ));
	
			assertTrue(rs.next());
			t = rs.getTimestamp(1);
			assertNotNull(t);
			assertTrue(t.equals(TS3WTZ));
	
			assertTrue(rs.next());
			t = rs.getTimestamp(1);
			assertNotNull(t);
			assertTrue(t.equals(TS4WTZ));
		}
		
		// Testing for Date
		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpDate1.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpDate2.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpDate3.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpDate4.getTime());
		
		// Testing for Time
		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpTime1.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpTime2.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpTime3.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpTime4.getTime());		

		assertTrue(! rs.next()); // end of table. Fail if more entries exist.

		rs.close();
		stmt.close();
	}

	/*
	 * Helper for the TimestampTests. It tests what should be in the db
	 */
	private void timestampTestWOTZ() throws SQLException
	{
		Statement stmt = con.createStatement();
		ResultSet rs;
		java.sql.Timestamp t;

		rs = stmt.executeQuery("select ts from " + TSWOTZ_TABLE); //removed the order by ts
		assertNotNull(rs);

		for (int i=0; i<3; i++)
		{
			assertTrue(rs.next());
			t = rs.getTimestamp(1);
			assertNotNull(t);
			assertTrue(t.equals(TS1WOTZ));
	
			assertTrue(rs.next());
			t = rs.getTimestamp(1);
			assertNotNull(t);
			assertTrue(t.equals(TS2WOTZ));
	
			assertTrue(rs.next());
			t = rs.getTimestamp(1);
			assertNotNull(t);
			assertTrue(t.equals(TS3WOTZ));
	
			assertTrue(rs.next());
			t = rs.getTimestamp(1);
			assertNotNull(t);
			assertTrue(t.equals(TS4WOTZ));
		}
		
		// Testing for Date
		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpDate1WOTZ.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpDate2WOTZ.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpDate3WOTZ.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpDate4WOTZ.getTime());
		
		// Testing for Time
		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpTime1WOTZ.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpTime2WOTZ.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpTime3WOTZ.getTime());

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertEquals(t.getTime(), tmpTime4WOTZ.getTime());		
		
		assertTrue(! rs.next()); // end of table. Fail if more entries exist.

		rs.close();
		stmt.close();
	}

	private static java.sql.Timestamp getTimestamp(int y, int m, int d, int h, int mn, int se, int f, String tz)
	{
		java.sql.Timestamp l_return = null;
		java.text.DateFormat l_df;
		try
		{
			String l_ts;
			l_ts = TestUtil.fix(y, 4) + "-" +
				   TestUtil.fix(m, 2) + "-" +
				   TestUtil.fix(d, 2) + " " +
				   TestUtil.fix(h, 2) + ":" +
				   TestUtil.fix(mn, 2) + ":" +
				   TestUtil.fix(se, 2) + " ";

			if (tz == null)
			{
				l_df = new java.text.SimpleDateFormat("y-M-d H:m:s");
			}
			else
			{
				l_ts = l_ts + tz;
				l_df = new java.text.SimpleDateFormat("y-M-d H:m:s z");
			}
			java.util.Date l_date = l_df.parse(l_ts);
			l_return = new java.sql.Timestamp(l_date.getTime());
			l_return.setNanos(f);
		}
		catch (Exception ex)
		{
			fail(ex.getMessage());
		}
		return l_return;
	}

	private static final java.sql.Timestamp TS1WTZ = getTimestamp(1950, 2, 7, 15, 0, 0, 100000000, "PST");
	private static final String TS1WTZ_PGFORMAT = "1950-02-07 15:00:00.1-08";

	private static final java.sql.Timestamp TS2WTZ = getTimestamp(2000, 2, 7, 15, 0, 0, 120000000, "GMT");
	private static final String TS2WTZ_PGFORMAT = "2000-02-07 15:00:00.12+00";

	private static final java.sql.Timestamp TS3WTZ = getTimestamp(2000, 7, 7, 15, 0, 0, 123000000, "GMT");
	private static final String TS3WTZ_PGFORMAT = "2000-07-07 15:00:00.123+00";

	private static final java.sql.Timestamp TS4WTZ = getTimestamp(2000, 7, 7, 15, 0, 0, 123456000, "GMT");
	private static final String TS4WTZ_PGFORMAT = "2000-07-07 15:00:00.123456+00";


	private static final java.sql.Timestamp TS1WOTZ = getTimestamp(1950, 2, 7, 15, 0, 0, 100000000, null);
	private static final String TS1WOTZ_PGFORMAT = "1950-02-07 15:00:00.1";

	private static final java.sql.Timestamp TS2WOTZ = getTimestamp(2000, 2, 7, 15, 0, 0, 120000000, null);
	private static final String TS2WOTZ_PGFORMAT = "2000-02-07 15:00:00.12";

	private static final java.sql.Timestamp TS3WOTZ = getTimestamp(2000, 7, 7, 15, 0, 0, 123000000, null);
	private static final String TS3WOTZ_PGFORMAT = "2000-07-07 15:00:00.123";

	private static final java.sql.Timestamp TS4WOTZ = getTimestamp(2000, 7, 7, 15, 0, 0, 123456000, null);
	private static final String TS4WOTZ_PGFORMAT = "2000-07-07 15:00:00.123456";

	private static final String TSWTZ_TABLE = "testtimestampwtz";
	private static final String TSWOTZ_TABLE = "testtimestampwotz";
	
	private static final java.sql.Date tmpDate1 = new java.sql.Date(TS1WTZ.getTime());
	private static final java.sql.Time tmpTime1 = new java.sql.Time(TS1WTZ.getTime());
	private static final java.sql.Date tmpDate2 = new java.sql.Date(TS2WTZ.getTime());
	private static final java.sql.Time tmpTime2 = new java.sql.Time(TS2WTZ.getTime());
	private static final java.sql.Date tmpDate3 = new java.sql.Date(TS3WTZ.getTime());
	private static final java.sql.Time tmpTime3 = new java.sql.Time(TS3WTZ.getTime());
	private static final java.sql.Date tmpDate4 = new java.sql.Date(TS4WTZ.getTime());
	private static final java.sql.Time tmpTime4 = new java.sql.Time(TS4WTZ.getTime());	
	
	private static final java.sql.Date tmpDate1WOTZ = new java.sql.Date(TS1WOTZ.getTime());
	private static final java.sql.Time tmpTime1WOTZ = new java.sql.Time(TS1WOTZ.getTime());
	private static final java.sql.Date tmpDate2WOTZ = new java.sql.Date(TS2WOTZ.getTime());
	private static final java.sql.Time tmpTime2WOTZ = new java.sql.Time(TS2WOTZ.getTime());
	private static final java.sql.Date tmpDate3WOTZ = new java.sql.Date(TS3WOTZ.getTime());
	private static final java.sql.Time tmpTime3WOTZ = new java.sql.Time(TS3WOTZ.getTime());
	private static final java.sql.Date tmpDate4WOTZ = new java.sql.Date(TS4WOTZ.getTime());
	private static final java.sql.Time tmpTime4WOTZ = new java.sql.Time(TS4WOTZ.getTime());	


}
