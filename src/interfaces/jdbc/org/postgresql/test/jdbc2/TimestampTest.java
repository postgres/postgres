package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;

/*
 * $Id: TimestampTest.java,v 1.11 2003/05/29 04:39:48 barry Exp $
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

			// Fall through helper
			timestampTestWTZ();

			assertEquals(4, stmt.executeUpdate("DELETE FROM " + TSWTZ_TABLE));

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

			// Fall through helper
			timestampTestWTZ();

			assertEquals(4, stmt.executeUpdate("DELETE FROM " + TSWTZ_TABLE));

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

			// Fall through helper
			timestampTestWOTZ();

			assertEquals(4, stmt.executeUpdate("DELETE FROM " + TSWOTZ_TABLE));

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

			// Fall through helper
			timestampTestWOTZ();

			assertEquals(4, stmt.executeUpdate("DELETE FROM " + TSWOTZ_TABLE));

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

		rs = stmt.executeQuery("select ts from " + TSWTZ_TABLE + " order by ts");
		assertNotNull(rs);

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

		rs = stmt.executeQuery("select ts from " + TSWOTZ_TABLE + " order by ts");
		assertNotNull(rs);

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

}
