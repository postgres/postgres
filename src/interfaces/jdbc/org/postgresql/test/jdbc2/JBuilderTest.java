package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;
import java.math.BigDecimal;

/**
 * $Id: JBuilderTest.java,v 1.4 2001/10/25 05:59:59 momjian Exp $
 *
 * Some simple tests to check that the required components needed for JBuilder
 * stay working
 *
 */
public class JBuilderTest extends TestCase
{

	public JBuilderTest(String name)
	{
		super(name);
	}

	// Set up the fixture for this testcase: the tables for this test.
	protected void setUp() throws Exception
	{
		Connection con = JDBC2Tests.openDB();

		JDBC2Tests.createTable( con, "test_c",
								"source text,cost money,imageid int4" );

		JDBC2Tests.closeDB(con);
	}

	// Tear down the fixture for this test case.
	protected void tearDown() throws Exception
	{
		Connection con = JDBC2Tests.openDB();
		JDBC2Tests.dropTable(con, "test_c");
		JDBC2Tests.closeDB(con);
	}

	/**
	 * This tests that Money types work. JDBCExplorer barfs if this fails.
	 */
	public void testMoney()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			Statement st = con.createStatement();
			ResultSet rs = st.executeQuery("select cost from test_c");
			assertNotNull(rs);

			while (rs.next())
			{
				double bd = rs.getDouble(1);
			}

			rs.close();
			st.close();

			JDBC2Tests.closeDB(con);
		}
		catch (Exception ex)
		{
			fail(ex.getMessage());
		}
	}
}
