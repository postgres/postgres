package org.postgresql.test.jdbc2;

import junit.framework.TestCase;

public class ANTTest extends TestCase
{
	public ANTTest(String name)
	{
		super(name);
	}

	/*
	 * This tests the acceptsURL() method with a couple of good and badly formed
	 * jdbc urls
	 */
	public void testANT()
	{
		String url = System.getProperty("database");
		String usr = System.getProperty("username");
		String psw = System.getProperty("password");

		assertNotNull(url);
		assertNotNull(usr);
		assertNotNull(psw);

		assertTrue(! url.equals(""));
		assertTrue(! usr.equals(""));
	}
}
