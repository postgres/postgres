package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;

/*
 * $Id: MiscTest.java,v 1.9 2003/05/29 03:21:32 barry Exp $
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
			Connection con = TestUtil.openDB();

			Statement st = con.createStatement();
			ResultSet rs = st.executeQuery("select datname from pg_database");
			assertNotNull(rs);

			while (rs.next())
			{
				String s = rs.getString(1);
			}

			rs.close();
			st.close();

			TestUtil.closeDB(con);
		}
		catch (Exception ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testError()
	{
		Connection con = TestUtil.openDB();
		try
		{

			// transaction mode
			con.setAutoCommit(false);
			Statement stmt = con.createStatement();
			stmt.execute("select 1/0");
			fail( "Should not execute this, as a SQLException s/b thrown" );
			con.commit();
		}
		catch ( Exception ex )
		{}
		try
		{
			con.commit();
			con.close();
		}
		catch ( Exception ex)
		{}
	}

	public void xtestLocking()
	{

		try
		{
			Connection con = TestUtil.openDB();
			Connection con2 = TestUtil.openDB();

			TestUtil.createTable(con, "test_lock", "name text");
			Statement st = con.createStatement();
			Statement st2 = con2.createStatement();
			con.setAutoCommit(false);
			st.execute("lock table test_lock");
			st2.executeUpdate( "insert into test_lock ( name ) values ('hello')" );
			con.commit();
			TestUtil.dropTable(con, "test_lock");
			con.close();
		}
		catch ( Exception ex )
		{
			fail( ex.getMessage() );
		}
	}
}
