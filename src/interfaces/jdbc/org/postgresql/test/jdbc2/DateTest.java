package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;

/*
 * $Id: DateTest.java,v 1.6 2003/09/22 04:55:00 barry Exp $
 *
 * Some simple tests based on problems reported by users. Hopefully these will
 * help prevent previous problems from re-occuring ;-)
 *
 */
public class DateTest extends TestCase
{

	private Connection con;
	private boolean testingSetDate = false;

	public DateTest(String name)
	{
		super(name);
	}

	protected void setUp() throws Exception
	{
		con = TestUtil.openDB();
		TestUtil.createTable(con, "testdate", "dt date");
	}

	protected void tearDown() throws Exception
	{
		TestUtil.dropTable(con, "testdate");
		TestUtil.closeDB(con);
	}

	/*
	 * Tests the time methods in ResultSet
	 */
	public void testGetDate()
	{
		try
		{
			Statement stmt = con.createStatement();

			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1950-02-07'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1970-06-02'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1999-08-11'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'2001-02-13'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1950-04-02'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1970-11-30'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1988-01-01'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'2003-07-09'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1934-02-28'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1969-04-03'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1982-08-03'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'2012-03-15'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1912-05-01'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1971-12-15'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'1984-12-03'")));
			assertEquals(1, stmt.executeUpdate(TestUtil.insertSQL("testdate", "'2000-01-01'")));

			/* dateTest() contains all of the tests */
			dateTest();

			assertEquals(16, stmt.executeUpdate("DELETE FROM " + "testdate"));
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
	public void testSetDate()
	{
		try
		{
			Statement stmt = con.createStatement();
			PreparedStatement ps = con.prepareStatement(TestUtil.insertSQL("testdate", "?"));

			ps.setDate(1, makeDate(1950, 2, 7));
			assertEquals(1, ps.executeUpdate());

			ps.setDate(1, makeDate(1970, 6, 2));
			assertEquals(1, ps.executeUpdate());

			ps.setDate(1, makeDate(1999, 8, 11));
			assertEquals(1, ps.executeUpdate());

			ps.setDate(1, makeDate(2001, 2, 13));
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, java.sql.Timestamp.valueOf("1950-04-02 12:00:00"), java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, java.sql.Timestamp.valueOf("1970-11-30 3:00:00"), java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, java.sql.Timestamp.valueOf("1988-1-1 13:00:00"), java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, java.sql.Timestamp.valueOf("2003-07-09 12:00:00"), java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, "1934-02-28", java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, "1969-04-3", java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, "1982-08-03", java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, "2012-3-15", java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, java.sql.Date.valueOf("1912-5-1"), java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, java.sql.Date.valueOf("1971-12-15"), java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, java.sql.Date.valueOf("1984-12-03"), java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, java.sql.Date.valueOf("2000-1-1"), java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, "1944-4-04-01", java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, "1970-01-1-10", java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, "1982-12-14+13", java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.setObject(1, "2010-08-3+05", java.sql.Types.DATE);
			assertEquals(1, ps.executeUpdate());
			
			ps.close();

			// Need to set a flag so that the method knows there is an extra test.
			testingSetDate = true;
			// Fall through helper
			dateTest();
			testingSetDate = false;

			assertEquals(20, stmt.executeUpdate("DELETE FROM testdate"));
			stmt.close();
		}
		catch (Exception ex)
		{
			fail(ex.getMessage());
		}
	}

	/*
	 * Helper for the date tests. It tests what should be in the db
	 */
	private void dateTest() throws SQLException
	{
		Statement st = con.createStatement();
		ResultSet rs;
		java.sql.Date d;

		rs = st.executeQuery(TestUtil.selectSQL("testdate", "dt"));
		assertNotNull(rs);

		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1950, 2, 7));

		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1970, 6, 2));

		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1999, 8, 11));

		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(2001, 2, 13));

		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1950, 4, 2));

		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1970, 11, 30));		

		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1988, 1, 1));

		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(2003, 7, 9));
		
		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1934, 2, 28));						
				
		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1969, 4, 3));
		
		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1982, 8, 3));
		
		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(2012, 3, 15));	
		
		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1912, 5, 1));	

		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1971, 12, 15));
		
		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(1984, 12, 3));

		assertTrue(rs.next());
		d = rs.getDate(1);
		assertNotNull(d);
		assertEquals(d, makeDate(2000, 1, 1));
		
		//now we have to convert the date, cause I fed it a timezone. IF it used it. hence the check			
		if (testingSetDate)
		{
			assertTrue(rs.next());
			d = rs.getDate(1);
			assertNotNull(d);
			java.sql.Date tmpDate = java.sql.Date.valueOf("1944-4-4");
			int localoffset = java.util.Calendar.getInstance().getTimeZone().getRawOffset();
			if (java.util.Calendar.getInstance().getTimeZone().inDaylightTime(tmpDate))
			{
				localoffset += 60 * 60 * 1000;
			}			
			int Dateoffset = 60 * 60 * 1000;
			tmpDate.setTime(tmpDate.getTime() + Dateoffset + localoffset);
			assertEquals(d, makeDate(tmpDate.getYear() + 1900, tmpDate.getMonth()+1, tmpDate.getDate()));
			
			assertTrue(rs.next());
			d = rs.getDate(1);
			assertNotNull(d);
			tmpDate = java.sql.Date.valueOf("1970-1-1");
			localoffset = java.util.Calendar.getInstance().getTimeZone().getRawOffset();
			if (java.util.Calendar.getInstance().getTimeZone().inDaylightTime(tmpDate))
			{
				localoffset += 60 * 60 * 1000;
			}						
			Dateoffset = 10 * 60 * 60 * 1000;
			tmpDate.setTime(tmpDate.getTime() + Dateoffset + localoffset);
			assertEquals(d, makeDate(tmpDate.getYear() + 1900, tmpDate.getMonth()+1, tmpDate.getDate()));
			
			assertTrue(rs.next());
			d = rs.getDate(1);
			assertNotNull(d);
			tmpDate = java.sql.Date.valueOf("1982-12-14");
			localoffset = java.util.Calendar.getInstance().getTimeZone().getRawOffset();
			if (java.util.Calendar.getInstance().getTimeZone().inDaylightTime(tmpDate))
			{
				localoffset += 60 * 60 * 1000;
			}						
			Dateoffset = -13 * 60 * 60 * 1000;
			tmpDate.setTime(tmpDate.getTime() + Dateoffset + localoffset);
			assertEquals(d, makeDate(tmpDate.getYear() + 1900, tmpDate.getMonth()+1, tmpDate.getDate()));

			assertTrue(rs.next());
			d = rs.getDate(1);
			assertNotNull(d);
			tmpDate = java.sql.Date.valueOf("2010-08-03");
			localoffset = java.util.Calendar.getInstance().getTimeZone().getRawOffset();
			if (java.util.Calendar.getInstance().getTimeZone().inDaylightTime(tmpDate))
			{
				localoffset += 60 * 60 * 1000;
			}						
			Dateoffset = -5 * 60 * 60 * 1000;
			tmpDate.setTime(tmpDate.getTime() + Dateoffset + localoffset);
			assertEquals(d, makeDate(tmpDate.getYear() + 1900, tmpDate.getMonth()+1, tmpDate.getDate()));
		}
		
		assertTrue(!rs.next());

		rs.close();
		st.close();
	}

	private java.sql.Date makeDate(int y, int m, int d)
	{
		return java.sql.Date.valueOf(TestUtil.fix(y, 4) + "-" +
									 TestUtil.fix(m, 2) + "-" +
									 TestUtil.fix(d, 2));
	}
}
