package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;

import junit.framework.TestCase;

/*
 * ResultSet tests.
 */
public class ResultSetTest extends TestCase
{
	private Connection con;

	public ResultSetTest(String name)
	{
		super(name);
	}

	protected void setUp() throws Exception
	{
		con = TestUtil.openDB();
		Statement stmt = con.createStatement();

		TestUtil.createTable(con, "testrs", "id integer");

		stmt.executeUpdate("INSERT INTO testrs VALUES (1)");
		stmt.executeUpdate("INSERT INTO testrs VALUES (2)");
		stmt.executeUpdate("INSERT INTO testrs VALUES (3)");
		stmt.executeUpdate("INSERT INTO testrs VALUES (4)");
		stmt.executeUpdate("INSERT INTO testrs VALUES (6)");
		stmt.executeUpdate("INSERT INTO testrs VALUES (9)");

		stmt.close();
	}

	protected void tearDown() throws Exception
	{
		TestUtil.dropTable(con, "testrs");
		TestUtil.closeDB(con);
	}

	public void testAbsolute() throws Exception
	{
		Statement stmt = con.createStatement();
		ResultSet rs = stmt.executeQuery("SELECT * FROM testrs");

		assertTrue(rs.absolute( -1));
		assertEquals(6, rs.getRow());

		assertTrue(rs.absolute(1));
		assertEquals(1, rs.getRow());

		assertTrue(!rs.absolute( -10));
		assertEquals(0, rs.getRow());
		assertTrue(rs.next());
		assertEquals(1, rs.getRow());

		assertTrue(!rs.absolute(10));
		assertEquals(0, rs.getRow());
		assertTrue(rs.previous());
		assertEquals(6, rs.getRow());

		stmt.close();
	}
	public void testEmptyResult()
	{
		try
		{
			Statement stmt = con.createStatement();
			ResultSet rs = stmt.executeQuery("SELECT * FROM testrs where id=100");
			rs.beforeFirst();
			rs.afterLast();
			assertTrue(!rs.first());
			assertTrue(!rs.last());
			assertTrue(!rs.next());


		}
		catch ( Exception ex )
		{
			fail( ex.getMessage() );
		}

	}
}
