package org.postgresql.test;

import java.sql.*;
import junit.framework.TestCase;

/*
 * Utility class for JDBC tests
 */
public class TestUtil
{
	/*
	 * Returns the Test database JDBC URL
	 */
	public static String getURL()
	{
		return System.getProperty("database");
	}

	/*
	 * Returns the Postgresql username
	 */
	public static String getUser()
	{
		return System.getProperty("username");
	}

	/*
	 * Returns the user's password
	 */
	public static String getPassword()
	{
		return System.getProperty("password");
	}

	/*
	 * Helper - opens a connection.
	 */
	public static java.sql.Connection openDB()
	{
		try
		{
			Class.forName("org.postgresql.Driver");
			return java.sql.DriverManager.getConnection(getURL(), getUser(), getPassword());
		}
		catch (ClassNotFoundException ex)
		{
			TestCase.fail(ex.getMessage());
		}
		catch (SQLException ex)
		{
			TestCase.fail(ex.getMessage());
		}
		return null;
	}

	/*
	 * Helper - closes an open connection. This rewrites SQLException to a failed
	 * assertion. It's static so other classes can use it.
	 */
	public static void closeDB(Connection con)
	{
		try
		{
			if (con != null)
				con.close();
		}
		catch (SQLException ex)
		{
			TestCase.fail(ex.getMessage());
		}
	}

	/*
	 * Helper - creates a test table for use by a test
	 */
	public static void createTable(Connection con,
								   String table,
								   String columns)
	{
		try
		{
			Statement st = con.createStatement();
			try
			{
				// Drop the table
				dropTable(con, table);

				// Now create the table
				st.executeUpdate("create table " + table + " (" + columns + ")");
			}
			finally
			{
				st.close();
			}
		}
		catch (SQLException ex)
		{
			TestCase.fail(ex.getMessage());
		}
	}

	/*
	 * Helper - drops a table
	 */
	public static void dropTable(Connection con, String table)
	{
		try
		{
			Statement stmt = con.createStatement();
			try
			{
				String sql = "DROP TABLE " + table;
				if (con instanceof org.postgresql.jdbc1.AbstractJdbc1Connection && ((org.postgresql.jdbc1.AbstractJdbc1Connection)con).haveMinimumServerVersion("7.3")) {
					sql += " CASCADE ";
				}
				stmt.executeUpdate(sql);
			}
			catch (SQLException ex)
			{
				// Since every create table issues a drop table
				// it's easy to get a table doesn't exist error.
				// we want to ignore these, but if we're in a
				// transaction we need to restart.
				// If the test case wants to catch this error
				// itself it should issue the drop SQL directly.
				if (ex.getMessage().indexOf("does not exist") != -1) {
					if (!con.getAutoCommit()) {
						con.rollback();
					}

				}
			}
		}
		catch (SQLException ex)
		{
			TestCase.fail(ex.getMessage());
		}
	}

	/*
	 * Helper - generates INSERT SQL - very simple
	 */
	public static String insertSQL(String table, String values)
	{
		return insertSQL(table, null, values);
	}

	public static String insertSQL(String table, String columns, String values)
	{
		String s = "INSERT INTO " + table;

		if (columns != null)
			s = s + " (" + columns + ")";

		return s + " VALUES (" + values + ")";
	}

	/*
	 * Helper - generates SELECT SQL - very simple
	 */
	public static String selectSQL(String table, String columns)
	{
		return selectSQL(table, columns, null, null);
	}

	public static String selectSQL(String table, String columns, String where)
	{
		return selectSQL(table, columns, where, null);
	}

	public static String selectSQL(String table, String columns, String where, String other)
	{
		String s = "SELECT " + columns + " FROM " + table;

		if (where != null)
			s = s + " WHERE " + where;
		if (other != null)
			s = s + " " + other;

		return s;
	}

	/*
	 * Helper to prefix a number with leading zeros - ugly but it works...
	 * @param v value to prefix
	 * @param l number of digits (0-10)
	 */
	public static String fix(int v, int l)
	{
		String s = "0000000000".substring(0, l) + Integer.toString(v);
		return s.substring(s.length() - l);
	}
}
