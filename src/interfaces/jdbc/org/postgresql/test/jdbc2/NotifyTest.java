package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;

import org.postgresql.PGConnection;
import org.postgresql.PGNotification;

public class NotifyTest extends TestCase
{

	private Connection conn;

	public NotifyTest(String name)
	{
		super(name);
	}

	protected void setUp() throws SQLException
	{
		conn = TestUtil.openDB();
	}

	protected void tearDown() throws SQLException
	{
		TestUtil.closeDB(conn);
	}

	public void testNotify() throws SQLException
	{
		Statement stmt = conn.createStatement();
		stmt.executeUpdate("LISTEN mynotification");
		stmt.executeUpdate("NOTIFY mynotification");

		PGNotification notifications[] = ((org.postgresql.PGConnection)conn).getNotifications();
		assertNotNull(notifications);
		assertEquals(notifications.length, 1);
		assertEquals(notifications[0].getName(), "mynotification");

		stmt.close();
	}
}
