package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * $Id: DateTest.java,v 1.2 2001/09/23 04:11:14 momjian Exp $
 *
 * Some simple tests based on problems reported by users. Hopefully these will
 * help prevent previous problems from re-occuring ;-)
 *
 */
public class DateTest extends TestCase {

	private Connection con;
	
	public DateTest(String name) {
		super(name);
	}

	protected void setUp() throws Exception {
		con = JDBC2Tests.openDB();
		JDBC2Tests.createTable(con, "testdate", "dt date");
	}

	protected void tearDown() throws Exception {
		JDBC2Tests.dropTable(con, "testdate");
		JDBC2Tests.closeDB(con);
	}
	
	/**
	 * Tests the time methods in ResultSet
	 */
	public void testGetDate() {
		try {
			Statement stmt = con.createStatement();
			
			assertEquals(1, stmt.executeUpdate(JDBC2Tests.insertSQL("testdate", "'1950-02-07'")));
			assertEquals(1, stmt.executeUpdate(JDBC2Tests.insertSQL("testdate", "'1970-06-02'")));
			assertEquals(1, stmt.executeUpdate(JDBC2Tests.insertSQL("testdate", "'1999-08-11'")));
			assertEquals(1, stmt.executeUpdate(JDBC2Tests.insertSQL("testdate", "'2001-02-13'")));

			/* dateTest() contains all of the tests */
			dateTest();

			assertEquals(4, stmt.executeUpdate("DELETE FROM " + "testdate"));
			stmt.close();
		} catch(Exception ex) {
			fail(ex.getMessage());
		}
	}

	/**
	 * Tests the time methods in PreparedStatement
	 */
	public void testSetDate() {
		try {
			Statement stmt = con.createStatement();
			PreparedStatement ps = con.prepareStatement(JDBC2Tests.insertSQL("testdate", "?"));

			ps.setDate(1, makeDate(1950, 2, 7));
			assertEquals(1, ps.executeUpdate());

			ps.setDate(1, makeDate(1970, 6, 2));
			assertEquals(1, ps.executeUpdate());

			ps.setDate(1, makeDate(1999, 8, 11));
			assertEquals(1, ps.executeUpdate());

			ps.setDate(1, makeDate(2001, 2, 13));
			assertEquals(1, ps.executeUpdate());

			ps.close();

			// Fall through helper
			dateTest();

			assertEquals(4, stmt.executeUpdate("DELETE FROM testdate"));
			stmt.close();
		} catch(Exception ex) {
			fail(ex.getMessage());
		}
	}

	/**
	 * Helper for the date tests. It tests what should be in the db
	 */
	private void dateTest() throws SQLException {
		Statement st = con.createStatement();
		ResultSet rs;
		java.sql.Date d;

		rs = st.executeQuery(JDBC2Tests.selectSQL("testdate", "dt"));
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

		assertTrue(!rs.next());

		rs.close();
		st.close();
	}

	private java.sql.Date makeDate(int y, int m, int d) {
		return java.sql.Date.valueOf(JDBC2Tests.fix(y, 4) + "-" +
									 JDBC2Tests.fix(m, 2) + "-" +
									 JDBC2Tests.fix(d, 2));
	}
}
