package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * $Id: TimestampTest.java,v 1.4 2001/09/29 03:11:11 momjian Exp $
 *
 * This has been the most controversial pair of methods since 6.5 was released!
 *
 * From now on, any changes made to either getTimestamp or setTimestamp
 * MUST PASS this TestCase!!!
 *
 */
public class TimestampTest extends TestCase {

	private Connection con;

	public TimestampTest(String name) {
		super(name);
	}

	protected void setUp() throws Exception {
		con = JDBC2Tests.openDB();
		Statement stmt = con.createStatement();
		
		JDBC2Tests.createTable(con, "testtimestamp", "ts timestamp");
	}

	protected void tearDown() throws Exception {
		JDBC2Tests.dropTable(con, "testtimestamp");
		JDBC2Tests.closeDB(con);
	}

	/**
	 * Tests the time methods in ResultSet
	 */
	public void testGetTimestamp() {
		try {
			Statement stmt = con.createStatement();

			assertEquals(1, stmt.executeUpdate(JDBC2Tests.insertSQL("testtimestamp",
																	"'1950-02-07 15:00:00'")));

			assertEquals(1, stmt.executeUpdate(JDBC2Tests.insertSQL("testtimestamp", "'" +
																	getTimestamp(1970, 6, 2, 8, 13, 0, 0).toString() +
																	"'")));

			assertEquals(1, stmt.executeUpdate(JDBC2Tests.insertSQL("testtimestamp",
																	"'1970-06-02 08:13:00'")));

			// Fall through helper
			timestampTest();

			assertEquals(3, stmt.executeUpdate("DELETE FROM testtimestamp"));

			stmt.close();
		} catch(Exception ex) {
			fail(ex.getMessage());
		}
	}

	/**
	 * Tests the time methods in PreparedStatement
	 */
	public void testSetTimestamp() {
		try {
			Statement stmt = con.createStatement();
			PreparedStatement pstmt = con.prepareStatement(JDBC2Tests.insertSQL("testtimestamp", "?"));

			pstmt.setTimestamp(1, getTimestamp(1950, 2, 7, 15, 0, 0, 0));
			assertEquals(1, pstmt.executeUpdate());

			pstmt.setTimestamp(1, getTimestamp(1970, 6, 2, 8, 13, 0, 0));
			assertEquals(1, pstmt.executeUpdate());

			pstmt.setTimestamp(1, getTimestamp(1970, 6, 2, 8, 13, 0, 0));
			assertEquals(1, pstmt.executeUpdate());

			// Fall through helper
			timestampTest();

			assertEquals(3, stmt.executeUpdate("DELETE FROM testtimestamp"));

			pstmt.close();
			stmt.close();
		} catch(Exception ex) {
			fail(ex.getMessage());
		}
	}

	/**
	 * Helper for the TimeTests. It tests what should be in the db
	 */
	private void timestampTest() throws SQLException {
		Statement stmt = con.createStatement();
		ResultSet rs;
		java.sql.Timestamp t;

		rs = stmt.executeQuery(JDBC2Tests.selectSQL("testtimestamp", "ts"));
		assertNotNull(rs);

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertTrue(t.equals(getTimestamp(1950, 2, 7, 15, 0, 0, 0)));

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertTrue(t.equals(getTimestamp(1970, 6, 2, 8, 13, 0, 0)));

		assertTrue(rs.next());
		t = rs.getTimestamp(1);
		assertNotNull(t);
		assertTrue(t.equals(getTimestamp(1970, 6, 2, 8, 13, 0, 0)));
		
		assertTrue(! rs.next()); // end of table. Fail if more entries exist.

		rs.close();
		stmt.close();
	}

	private java.sql.Timestamp getTimestamp(int y, int m, int d, int h, int mn, int se, int f) {
		return java.sql.Timestamp.valueOf(JDBC2Tests.fix(y,  4) + "-" +
										  JDBC2Tests.fix(m,  2) + "-" +
										  JDBC2Tests.fix(d,  2) + " " +
										  JDBC2Tests.fix(h,  2) + ":" +
										  JDBC2Tests.fix(mn, 2) + ":" +
										  JDBC2Tests.fix(se, 2) + "." +
										  JDBC2Tests.fix(f,  9));
	}
}
