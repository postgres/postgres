package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.io.*;
import java.sql.*;

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
		con = JDBC2Tests.openDB();
		Statement stmt = con.createStatement();

		JDBC2Tests.createTable(con, "testrs", "id integer");

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
		JDBC2Tests.dropTable(con, "testrs");
		JDBC2Tests.closeDB(con);
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
}
