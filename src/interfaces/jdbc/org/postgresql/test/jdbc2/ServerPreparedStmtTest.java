package org.postgresql.test.jdbc2;

import org.postgresql.PGStatement;
import org.postgresql.test.TestUtil;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.Statement;

import junit.framework.TestCase;

/*
 *  Tests for using server side prepared statements
 */
public class ServerPreparedStmtTest extends TestCase
{
	private Connection con;

	public ServerPreparedStmtTest(String name)
	{
		super(name);
	}

	protected void setUp() throws Exception
	{
		con = TestUtil.openDB();
		Statement stmt = con.createStatement();

		TestUtil.createTable(con, "testsps", "id integer, value boolean");

		stmt.executeUpdate("INSERT INTO testsps VALUES (1,'t')");
		stmt.executeUpdate("INSERT INTO testsps VALUES (2,'t')");
		stmt.executeUpdate("INSERT INTO testsps VALUES (3,'t')");
		stmt.executeUpdate("INSERT INTO testsps VALUES (4,'t')");
		stmt.executeUpdate("INSERT INTO testsps VALUES (6,'t')");
		stmt.executeUpdate("INSERT INTO testsps VALUES (9,'f')");

		stmt.close();
	}

	protected void tearDown() throws Exception
	{
		TestUtil.dropTable(con, "testsps");
		TestUtil.closeDB(con);
	}

	public void testPreparedStatementsNoBinds() throws Exception
	{
		PreparedStatement pstmt = con.prepareStatement("SELECT * FROM testsps WHERE id = 2");
        ((PGStatement)pstmt).setUseServerPrepare(true);
        if (TestUtil.haveMinimumServerVersion(con,"7.3")) {
			assertTrue(((PGStatement)pstmt).isUseServerPrepare());
		} else {
			assertTrue(!((PGStatement)pstmt).isUseServerPrepare());
		}

        //Test that basic functionality works
		ResultSet rs = pstmt.executeQuery();
		assertTrue(rs.next());
        assertEquals(2, rs.getInt(1));
        rs.close();

        //Verify that subsequent calls still work
		rs = pstmt.executeQuery();
		assertTrue(rs.next());
        assertEquals(2, rs.getInt(1));
        rs.close();

        //Verify that using the statement to execute a different query works
		rs = pstmt.executeQuery("SELECT * FROM testsps WHERE id = 9");
		assertTrue(rs.next());
        assertEquals(9, rs.getInt(1));
        rs.close();

        ((PGStatement)pstmt).setUseServerPrepare(false);
        assertTrue(!((PGStatement)pstmt).isUseServerPrepare());

        //Verify that using the statement still works after turning off prepares
		rs = pstmt.executeQuery("SELECT * FROM testsps WHERE id = 9");
		assertTrue(rs.next());
        assertEquals(9, rs.getInt(1));
        rs.close();

		pstmt.close();
	}

	public void testPreparedStatementsWithOneBind() throws Exception
	{
		PreparedStatement pstmt = con.prepareStatement("SELECT * FROM testsps WHERE id = ?");
        ((PGStatement)pstmt).setUseServerPrepare(true);
        if (TestUtil.haveMinimumServerVersion(con,"7.3")) {
			assertTrue(((PGStatement)pstmt).isUseServerPrepare());
		} else {
			assertTrue(!((PGStatement)pstmt).isUseServerPrepare());
		}

        //Test that basic functionality works
        pstmt.setInt(1,2);
		ResultSet rs = pstmt.executeQuery();
		assertTrue(rs.next());
        assertEquals(2, rs.getInt(1));
        rs.close();

        //Verify that subsequent calls still work
		rs = pstmt.executeQuery();
		assertTrue(rs.next());
        assertEquals(2, rs.getInt(1));
        rs.close();

        //Verify that using the statement to execute a different query works
		rs = pstmt.executeQuery("SELECT * FROM testsps WHERE id = 9");
		assertTrue(rs.next());
        assertEquals(9, rs.getInt(1));
        rs.close();

        ((PGStatement)pstmt).setUseServerPrepare(false);
        assertTrue(!((PGStatement)pstmt).isUseServerPrepare());

        //Verify that using the statement still works after turning off prepares
		rs = pstmt.executeQuery("SELECT * FROM testsps WHERE id = 9");
		assertTrue(rs.next());
        assertEquals(9, rs.getInt(1));
        rs.close();

		pstmt.close();
	}

	// Verify we can bind booleans-as-objects ok.
	public void testBooleanObjectBind() throws Exception
	{
		PreparedStatement pstmt = con.prepareStatement("SELECT * FROM testsps WHERE value = ?");
        ((PGStatement)pstmt).setUseServerPrepare(true);
        if (TestUtil.haveMinimumServerVersion(con,"7.3")) {
			assertTrue(((PGStatement)pstmt).isUseServerPrepare());
		} else {
			assertTrue(!((PGStatement)pstmt).isUseServerPrepare());
		}

        pstmt.setObject(1, new Boolean(false), java.sql.Types.BIT);
		ResultSet rs = pstmt.executeQuery();
		assertTrue(rs.next());
        assertEquals(9, rs.getInt(1));
        rs.close();
	}

	// Verify we can bind booleans-as-integers ok.
	public void testBooleanIntegerBind() throws Exception
	{
		PreparedStatement pstmt = con.prepareStatement("SELECT * FROM testsps WHERE id = ?");
        ((PGStatement)pstmt).setUseServerPrepare(true);
        if (TestUtil.haveMinimumServerVersion(con,"7.3")) {
			assertTrue(((PGStatement)pstmt).isUseServerPrepare());
		} else {
			assertTrue(!((PGStatement)pstmt).isUseServerPrepare());
		}

        pstmt.setObject(1, new Boolean(true), java.sql.Types.INTEGER);
		ResultSet rs = pstmt.executeQuery();
		assertTrue(rs.next());
        assertEquals(1, rs.getInt(1));
        rs.close();
	}

	// Verify we can bind booleans-as-native-types ok.
	public void testBooleanBind() throws Exception
	{
		PreparedStatement pstmt = con.prepareStatement("SELECT * FROM testsps WHERE value = ?");
        ((PGStatement)pstmt).setUseServerPrepare(true);
        if (TestUtil.haveMinimumServerVersion(con,"7.3")) {
			assertTrue(((PGStatement)pstmt).isUseServerPrepare());
		} else {
			assertTrue(!((PGStatement)pstmt).isUseServerPrepare());
		}

        pstmt.setBoolean(1, false);
		ResultSet rs = pstmt.executeQuery();
		assertTrue(rs.next());
        assertEquals(9, rs.getInt(1));
        rs.close();
	}

	public void testPreparedStatementsWithBinds() throws Exception
	{
		PreparedStatement pstmt = con.prepareStatement("SELECT * FROM testsps WHERE id = ? or id = ?");
        ((PGStatement)pstmt).setUseServerPrepare(true);
        if (TestUtil.haveMinimumServerVersion(con,"7.3")) {
			assertTrue(((PGStatement)pstmt).isUseServerPrepare());
		} else {
			assertTrue(!((PGStatement)pstmt).isUseServerPrepare());
		}

        //Test that basic functionality works
        //bind different datatypes
        pstmt.setInt(1,2);
        pstmt.setString(2,"2");
		ResultSet rs = pstmt.executeQuery();
		assertTrue(rs.next());
        assertEquals(2, rs.getInt(1));
        rs.close();

        //Verify that subsequent calls still work
		rs = pstmt.executeQuery();
		assertTrue(rs.next());
        assertEquals(2, rs.getInt(1));
        rs.close();

		pstmt.close();
	}

	public void testSPSToggle() throws Exception
	{
		// Verify we can toggle UseServerPrepare safely before a query is executed.
		PreparedStatement pstmt = con.prepareStatement("SELECT * FROM testsps WHERE id = 2");
        ((PGStatement)pstmt).setUseServerPrepare(true);
        ((PGStatement)pstmt).setUseServerPrepare(false);
	}

	public void testBytea() throws Exception
	{
		// Verify we can use setBytes() with a server-prepared update.
		try {
			TestUtil.createTable(con, "testsps_bytea", "data bytea");
			
			PreparedStatement pstmt = con.prepareStatement("INSERT INTO testsps_bytea(data) VALUES (?)");
			((PGStatement)pstmt).setUseServerPrepare(true);
			pstmt.setBytes(1, new byte[100]);
			pstmt.executeUpdate();
		} finally {
			TestUtil.dropTable(con, "testsps_bytea");
		}
	}

	// Check statements are not transformed when they shouldn't be.
	public void TODO_FAILS_testCreateTable() throws Exception {
		// CREATE TABLE isn't supported by PREPARE; the driver should realize this and
		// still complete without error.
		PreparedStatement pstmt = con.prepareStatement("CREATE TABLE testsps_bad(data int)");
		((PGStatement)pstmt).setUseServerPrepare(true);
		pstmt.executeUpdate();
		TestUtil.dropTable(con, "testsps_bad");
	}

	public void TODO_FAILS_testMultistatement() throws Exception {
		// Shouldn't try to PREPARE this one, if we do we get:
		//   PREPARE x(int,int) AS INSERT .... $1 ; INSERT ... $2    -- syntax error
		try {
			TestUtil.createTable(con, "testsps_multiple", "data int");
			PreparedStatement pstmt = con.prepareStatement("INSERT INTO testsps_multiple(data) VALUES (?); INSERT INTO testsps_multiple(data) VALUES (?)");
			((PGStatement)pstmt).setUseServerPrepare(true);			
			pstmt.setInt(1, 1);
			pstmt.setInt(2, 2);
			pstmt.executeUpdate(); // Two inserts.

			pstmt.setInt(1, 3);
			pstmt.setInt(2, 4);
			pstmt.executeUpdate(); // Two more inserts.
			
			ResultSet check = con.createStatement().executeQuery("SELECT COUNT(*) FROM testsps_multiple");
			assertTrue(check.next());
			assertEquals(4, check.getInt(1));
		} finally {
			TestUtil.dropTable(con, "testsps_multiple");
		}
	}
}
