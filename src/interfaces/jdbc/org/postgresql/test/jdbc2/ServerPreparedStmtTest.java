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

		TestUtil.createTable(con, "testsps", "id integer");

		stmt.executeUpdate("INSERT INTO testsps VALUES (1)");
		stmt.executeUpdate("INSERT INTO testsps VALUES (2)");
		stmt.executeUpdate("INSERT INTO testsps VALUES (3)");
		stmt.executeUpdate("INSERT INTO testsps VALUES (4)");
		stmt.executeUpdate("INSERT INTO testsps VALUES (6)");
		stmt.executeUpdate("INSERT INTO testsps VALUES (9)");

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

}
