package org.postgresql.test;

import junit.framework.TestSuite;
import junit.framework.TestCase;

import org.postgresql.test.jdbc2.*;
import java.sql.*;

/*
 * Executes all known tests for JDBC2 and includes some utility methods.
 */
public class JDBC2Tests extends TestSuite
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
			return java.sql.DriverManager.getConnection(JDBC2Tests.getURL(), JDBC2Tests.getUser(), JDBC2Tests.getPassword());
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
				stmt.executeUpdate("DROP TABLE " + table);
			}
			catch (SQLException ex)
			{
				// ignore
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

	/*
	 * The main entry point for JUnit
	 */
	public static TestSuite suite()
	{
		TestSuite suite = new TestSuite();

		//
		// Add one line per class in our test cases. These should be in order of
		// complexity.

		// ANTTest should be first as it ensures that test parameters are
		// being sent to the suite. It also initialises the database (if required)
		// with some simple global tables (will make each testcase use its own later).
		//
		suite.addTestSuite(ANTTest.class);

		// Basic Driver internals
		suite.addTestSuite(DriverTest.class);
		suite.addTestSuite(ConnectionTest.class);
		suite.addTestSuite(DatabaseMetaDataTest.class);
		suite.addTestSuite(EncodingTest.class);

		// Connectivity/Protocols

		// ResultSet
		suite.addTestSuite(ResultSetTest.class);

		// Time, Date, Timestamp
		suite.addTestSuite(DateTest.class);
		suite.addTestSuite(TimeTest.class);
		suite.addTestSuite(TimestampTest.class);

		// PreparedStatement

		// BatchExecute
		suite.addTestSuite(BatchExecuteTest.class);

		// MetaData

		// Other misc tests, based on previous problems users have had or specific
		// features some applications require.
		suite.addTestSuite(JBuilderTest.class);
		suite.addTestSuite(MiscTest.class);

		// Fastpath/LargeObject
		suite.addTestSuite(BlobTest.class);

		// That's all folks
		return suite;
	}
}
