package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;

/*
 * $Id: TimeTest.java,v 1.6 2003/09/22 04:55:00 barry Exp $
 *
 * Some simple tests based on problems reported by users. Hopefully these will
 * help prevent previous problems from re-occuring ;-)
 *
 */
public class TimeTest extends TestCase
{

	private Connection con;
	private boolean testSetTime = false;

	public TimeTest(String name)
	{
		super(name);
	}

	protected void setUp() throws Exception
	{
		con = TestUtil.openDB();
		TestUtil.createTable(con, "testtime", "tm time");
	}

	protected void tearDown() throws Exception
	{
		TestUtil.dropTable(con, "testtime");
		TestUtil.closeDB(con);
	}

	/*
	 * Tests the time methods in ResultSet
	 */
	public void testGetTime()
	{
		try
		{
			Statement stmt = con.createStatement();

			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testtime", "'01:02:03'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testtime", "'23:59:59'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testtime", "'12:00:00'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testtime", "'05:15:21'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testtime", "'16:21:51'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testtime", "'12:15:12'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testtime", "'22:12:01'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testtime", "'08:46:44'")));
			

			// Fall through helper
			timeTest();

			assertEquals(8, stmt.executeUpdate("DELETE FROM testtime"));
			stmt.close();
		}
		catch (Exception ex)
		{
			fail(ex.getMessage());
		}
	}

	/*
	 * Tests the time methods in PreparedStatement
	 */
	public void testSetTime()
	{
		try
		{
			PreparedStatement ps = con.prepareStatement(TestUtil.insertSQL("testtime", "?"));
			Statement stmt = con.createStatement();

			ps.setTime(1, makeTime(1, 2, 3));
			assertEquals(1, ps.executeUpdate());

			ps.setTime(1, makeTime(23, 59, 59));
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, java.sql.Time.valueOf("12:00:00"), java.sql.Types.TIME);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, java.sql.Time.valueOf("05:15:21"), java.sql.Types.TIME);
			assertEquals(1, ps.executeUpdate());			

			ps.setObject(1, java.sql.Time.valueOf("16:21:51"), java.sql.Types.TIME);
			assertEquals(1, ps.executeUpdate());

			ps.setObject(1, java.sql.Time.valueOf("12:15:12"), java.sql.Types.TIME);
			assertEquals(1, ps.executeUpdate());

			ps.setObject(1, "22:12:1", java.sql.Types.TIME);
			assertEquals(1, ps.executeUpdate());

			ps.setObject(1, "8:46:44", java.sql.Types.TIME);			
			assertEquals(1, ps.executeUpdate());			

			ps.setObject(1, "5:1:2-03", java.sql.Types.TIME);
			assertEquals(1, ps.executeUpdate());

			ps.setObject(1, "23:59:59+11", java.sql.Types.TIME);
			assertEquals(1, ps.executeUpdate());			

			// Need to let the test know this one has extra test cases.
			testSetTime = true;
			// Fall through helper
			timeTest();
			testSetTime = false;

			assertEquals(10, stmt.executeUpdate("DELETE FROM testtime"));
			stmt.close();
			ps.close();
		}
		catch (Exception ex)
		{
			fail(ex.getMessage());
		}
	}

	/*
	 * Helper for the TimeTests. It tests what should be in the db
	 */
	private void timeTest() throws SQLException
	{
		Statement st = con.createStatement();
		ResultSet rs;
		java.sql.Time t;

		rs = st.executeQuery(TestUtil.selectSQL("testtime", "tm"));
		assertNotNull(rs);

		assertTrue(rs.next());
		t = rs.getTime(1);
		assertNotNull(t);
		assertEquals(makeTime(1, 2, 3), t);

		assertTrue(rs.next());
		t = rs.getTime(1);
		assertNotNull(t);
		assertEquals(makeTime(23, 59, 59), t);

		assertTrue(rs.next());
		t = rs.getTime(1);
		assertNotNull(t);
		assertEquals(makeTime(12, 0, 0), t);

		assertTrue(rs.next());
		t = rs.getTime(1);
		assertNotNull(t);
		assertEquals(makeTime(5, 15, 21), t);

		assertTrue(rs.next());
		t = rs.getTime(1);
		assertNotNull(t);
		assertEquals(makeTime(16, 21, 51), t);

		assertTrue(rs.next());
		t = rs.getTime(1);
		assertNotNull(t);
		assertEquals(makeTime(12, 15, 12), t);

		assertTrue(rs.next());
		t = rs.getTime(1);
		assertNotNull(t);
		assertEquals(makeTime(22, 12, 1), t);

		assertTrue(rs.next());
		t = rs.getTime(1);
		assertNotNull(t);
		assertEquals(makeTime(8, 46, 44), t);
		
		// If we're checking for timezones.
		if (testSetTime)
		{
			assertTrue(rs.next());
			t = rs.getTime(1);
			assertNotNull(t);
			java.sql.Time tmpTime = java.sql.Time.valueOf("5:1:2");
			int localoffset = java.util.Calendar.getInstance().getTimeZone().getRawOffset();
			if (java.util.Calendar.getInstance().getTimeZone().inDaylightTime(tmpTime))
			{
				localoffset += 60 * 60 * 1000;
			}
			int Timeoffset = 3 * 60 * 60 * 1000;
			tmpTime.setTime(tmpTime.getTime() + Timeoffset + localoffset);
			assertEquals(t, makeTime(tmpTime.getHours(), tmpTime.getMinutes(), tmpTime.getSeconds()));
			
			assertTrue(rs.next());
			t = rs.getTime(1);
			assertNotNull(t);
			tmpTime= java.sql.Time.valueOf("23:59:59");
			localoffset = java.util.Calendar.getInstance().getTimeZone().getRawOffset();
			if (java.util.Calendar.getInstance().getTimeZone().inDaylightTime(tmpTime))
			{
				localoffset += 60 * 60 * 1000;
			}			
			Timeoffset = -11 * 60 * 60 * 1000;
			tmpTime.setTime(tmpTime.getTime() + Timeoffset + localoffset);
			assertEquals(t, makeTime(tmpTime.getHours(), tmpTime.getMinutes(), tmpTime.getSeconds()));
		}
						
		assertTrue(! rs.next());

		rs.close();
	}

	private java.sql.Time makeTime(int h, int m, int s)
	{
		return java.sql.Time.valueOf(TestUtil.fix(h, 2) + ":" +
									 TestUtil.fix(m, 2) + ":" +
									 TestUtil.fix(s, 2));
	}
}
