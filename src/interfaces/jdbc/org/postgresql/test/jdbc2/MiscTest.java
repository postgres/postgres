package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;
import java.io.*;

/*
 * $Id: MiscTest.java,v 1.10.4.1 2003/12/11 03:59:37 davec Exp $
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
				rs.getString(1);
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
		catch ( SQLException ex )
		{
			// Verify that the SQLException is serializable.
			try {
				ByteArrayOutputStream baos = new ByteArrayOutputStream();
				ObjectOutputStream oos = new ObjectOutputStream(baos);
				oos.writeObject(ex);
				oos.close();
			} catch (IOException ioe) {
				fail(ioe.getMessage());
			}
		}
		try
		{
			con.commit();
			con.close();
		}
		catch ( Exception ex)
		{}
	}
	public void testLastOID()
	{
		Connection con = null;
		try
		{
			con = TestUtil.openDB();
			TestUtil.createTable( con, "testoid","id int");

			Statement stmt = con.createStatement();
			con.setAutoCommit(false);
			stmt.executeUpdate( "insert into testoid values (1)" );
			con.commit();
			long insertedOid = ((org.postgresql.PGStatement)stmt).getLastOID();
			con.setAutoCommit(true);
			TestUtil.dropTable( con, "testoid");
		}
		catch ( Exception ex )
		{
			fail( ex.getMessage() );
		}
		finally
		{
			try{if (con !=null )con.close();}catch(Exception ex){}
		}
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
