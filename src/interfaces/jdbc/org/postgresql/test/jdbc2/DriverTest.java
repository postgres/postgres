package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;

/*
 * $Id: DriverTest.java,v 1.5 2002/08/14 20:35:40 barry Exp $
 *
 * Tests the dynamically created class org.postgresql.Driver
 *
 */
public class DriverTest extends TestCase
{

	public DriverTest(String name)
	{
		super(name);
	}

	/*
	 * This tests the acceptsURL() method with a couple of good and badly formed
	 * jdbc urls
	 */
	public void testAcceptsURL()
	{
		try
		{

			// Load the driver (note clients should never do it this way!)
			org.postgresql.Driver drv = new org.postgresql.Driver();
			assertNotNull(drv);

			// These are always correct
			assertTrue(drv.acceptsURL("jdbc:postgresql:test"));
			assertTrue(drv.acceptsURL("jdbc:postgresql://localhost/test"));
			assertTrue(drv.acceptsURL("jdbc:postgresql://localhost:5432/test"));
			assertTrue(drv.acceptsURL("jdbc:postgresql://127.0.0.1/anydbname"));
			assertTrue(drv.acceptsURL("jdbc:postgresql://127.0.0.1:5433/hidden"));

			// Badly formatted url's
			assertTrue(!drv.acceptsURL("jdbc:postgres:test"));
			assertTrue(!drv.acceptsURL("postgresql:test"));

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	/*
	 * Tests parseURL (internal)
	 */
	/*
	 * Tests the connect method by connecting to the test database
	 */
	public void testConnect()
	{
		Connection con = null;
		try
		{
			Class.forName("org.postgresql.Driver");

			// Test with the url, username & password
			con = DriverManager.getConnection(TestUtil.getURL(), TestUtil.getUser(), TestUtil.getPassword());
			assertNotNull(con);
			con.close();

			// Test with the username in the url
			con = DriverManager.getConnection(TestUtil.getURL() + "?user=" + TestUtil.getUser() + "&password=" + TestUtil.getPassword());
			assertNotNull(con);
			con.close();
		}
		catch (ClassNotFoundException ex)
		{
			fail(ex.getMessage());
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}
}
