package example;

import java.io.*;
import java.sql.*;

/*
 *
 * $Id: basic.java,v 1.14 2003/09/09 11:24:04 barry Exp $
 *
 * This example tests the basic components of the JDBC driver, and shows
 * how even the simplest of queries can be implemented.
 *
 * To use this example, you need a database to be in existence. This example
 * will create a table called basic.
 *
 * Note: This will only work with post 7.0 drivers.
 *
 */

public class basic
{
	Connection db;	// The connection to the database
	Statement st;	// Our statement to run queries with

	public basic(String args[]) throws ClassNotFoundException, FileNotFoundException, IOException, SQLException
	{
		String url = args[0];
		String usr = args[1];
		String pwd = args[2];

		// Load the driver
		Class.forName("org.postgresql.Driver");

		// Connect to database
		System.out.println("Connecting to Database URL = " + url);
		db = DriverManager.getConnection(url, usr, pwd);

		System.out.println("Connected...Now creating a statement");
		st = db.createStatement();

		// Clean up the database (in case we failed earlier) then initialise
		cleanup();

		// Now run tests using JDBC methods
		doexample();

		// Clean up the database
		cleanup();

		// Finally close the database
		System.out.println("Now closing the connection");
		st.close();
		db.close();

		//throw postgresql.Driver.notImplemented();
	}

	/*
	 * This drops the table (if it existed). No errors are reported.
	 */
	public void cleanup()
	{
		try
		{
			st.executeUpdate("drop table basic");
		}
		catch (Exception ex)
		{
			// We ignore any errors here
		}
	}

	/*
	 * This performs the example
	 */
	public void doexample() throws SQLException
	{
		System.out.println("\nRunning tests:");

		// First we need a table to store data in
		st.executeUpdate("create table basic (a int2, b int2)");

		// Now insert some data, using the Statement
		st.executeUpdate("insert into basic values (1,1)");
		st.executeUpdate("insert into basic values (2,1)");
		st.executeUpdate("insert into basic values (3,1)");

		// This shows how to get the oid of a just inserted row
		st.executeUpdate("insert into basic values (4,1)");
		long insertedOID = ((org.postgresql.PGStatement)st).getLastOID();
		System.out.println("Inserted row with oid " + insertedOID);

		// Now change the value of b from 1 to 8
		st.executeUpdate("update basic set b=8");
		System.out.println("Updated " + st.getUpdateCount() + " rows");

		// Now delete 2 rows
		st.executeUpdate("delete from basic where a<3");
		System.out.println("deleted " + st.getUpdateCount() + " rows");

		// For large inserts, a PreparedStatement is more efficient, because it
		// supports the idea of precompiling the SQL statement, and to store
		// directly, a Java object into any column. PostgreSQL doesnt support
		// precompiling, but does support setting a column to the value of a
		// Java object (like Date, String, etc).
		//
		// Also, this is the only way of writing dates in a datestyle independent
		// manner. (DateStyles are PostgreSQL's way of handling different methods
		// of representing dates in the Date data type.)
		PreparedStatement ps = db.prepareStatement("insert into basic values (?,?)");
		for (int i = 2;i < 5;i++)
		{
			ps.setInt(1, 4);		// "column a" = 5
			ps.setInt(2, i);		// "column b" = i
			ps.executeUpdate(); // executeUpdate because insert returns no data
		}
		ps.close();			// Always close when we are done with it

		// Finally perform a query on the table
		System.out.println("performing a query");
		ResultSet rs = st.executeQuery("select a, b from basic");
		if (rs != null)
		{
			// Now we run through the result set, printing out the result.
			// Note, we must call .next() before attempting to read any results
			while (rs.next())
			{
				int a = rs.getInt("a"); // This shows how to get the value by name
				int b = rs.getInt(2);	// This shows how to get the value by column
				System.out.println("  a=" + a + " b=" + b);
			}
			rs.close(); // again, you must close the result when done
		}

		// Now run the query again, showing a more efficient way of getting the
		// result if you don't know what column number a value is in



		System.out.println("performing another query");
		rs = st.executeQuery("select * from basic where b>1");
		if (rs != null)
		{
			// First find out the column numbers.
			//
			// It's best to do this here, as calling the methods with the column
			// numbers actually performs this call each time they are called. This
			// really speeds things up on large queries.
			//
			int col_a = rs.findColumn("a");
			int col_b = rs.findColumn("b");

			// Now we run through the result set, printing out the result.
			// Again, we must call .next() before attempting to read any results
			while (rs.next())
			{
				int a = rs.getInt(col_a); // This shows how to get the value by name
				int b = rs.getInt(col_b); // This shows how to get the value by column
				System.out.println("  a=" + a + " b=" + b);
			}
			rs.close(); // again, you must close the result when done
		}

		// Now test maxrows by setting it to 3 rows



		st.setMaxRows(3);
		System.out.println("performing a query limited to " + st.getMaxRows());
		rs = st.executeQuery("select a, b from basic");
		while (rs.next())
		{
			int a = rs.getInt("a"); // This shows how to get the value by name
			int b = rs.getInt(2);	// This shows how to get the value by column
			System.out.println("  a=" + a + " b=" + b);
		}
		rs.close(); // again, you must close the result when done

		// The last thing to do is to drop the table. This is done in the
		// cleanup() method.
	}

	/*
	 * Display some instructions on how to run the example
	 */
	public static void instructions()
	{
		System.out.println("\nThis example tests the basic components of the JDBC driver, demonstrating\nhow to build simple queries in java.\n");
		System.out.println("Useage:\n java example.basic jdbc:postgresql:database user password [debug]\n\nThe debug field can be anything. It's presence will enable DriverManager's\ndebug trace. Unless you want to see screens of items, don't put anything in\nhere.");
		System.exit(1);
	}

	/*
	 * This little lot starts the test
	 */
	public static void main(String args[])
	{
		System.out.println("PostgreSQL basic test v6.3 rev 1\n");

		if (args.length < 3)
			instructions();

		// This line outputs debug information to stderr. To enable this, simply
		// add an extra parameter to the command line
		if (args.length > 3)
			DriverManager.setLogStream(System.err);

		// Now run the tests
		try
		{
			basic test = new basic(args);
		}
		catch (Exception ex)
		{
			System.err.println("Exception caught.\n" + ex);
			ex.printStackTrace();
		}
	}
}
