package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/*
 * $Id: MiscTest.java,v 1.5 2002/05/30 16:26:55 davec Exp $
 *
 * Some simple tests based on problems reported by users. Hopefully these will
 * help prevent previous problems from re-occuring ;-)
 *
 */
public class MiscTest extends TestCase
{

	public MiscTest(String name)
	{
		super(name);
	}

	/*
	 * Some versions of the driver would return rs as a null?
	 *
	 * Sasha <ber0806@iperbole.bologna.it> was having this problem.
	 *
	 * Added Feb 13 2001
	 */
	public void testDatabaseSelectNullBug()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			Statement st = con.createStatement();
			ResultSet rs = st.executeQuery("select datname from pg_database");
			assertNotNull(rs);

			while (rs.next())
			{
				String s = rs.getString(1);
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

	public void xtestLocking()
	{

		System.out.println("testing lock");
		try
		{
			Connection con = JDBC2Tests.openDB();
			Connection con2 = JDBC2Tests.openDB();

			JDBC2Tests.createTable(con, "test_lock", "name text");
			Statement st = con.createStatement();
			Statement st2 = con2.createStatement();
			con.setAutoCommit(false);
			st.execute("lock table test_lock");
			st2.executeUpdate( "insert into test_lock ( name ) values ('hello')" );
 			con.commit();
			JDBC2Tests.dropTable(con, "test_lock");
			con.close();
		}
		catch ( Exception ex )
		{
			fail( ex.getMessage() );
		}
	}
}
