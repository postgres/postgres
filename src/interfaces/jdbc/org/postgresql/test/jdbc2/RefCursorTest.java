package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import java.sql.CallableStatement;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;
import java.sql.Types;

import junit.framework.TestCase;

/*
 * RefCursor ResultSet tests.
 * This test case is basically the same as the ResultSet test case.
 *
 * @author Nic Ferrier <nferrier@tapsellferrier.co.uk>
 */
public class RefCursorTest extends TestCase
{
	private Connection con;

	public RefCursorTest(String name)
	{
		super(name);
	}

	protected void setUp() throws Exception
	{
                // this is the same as the ResultSet setup.
		con = TestUtil.openDB();
		Statement stmt = con.createStatement();

		TestUtil.createTable(con, "testrs", "id integer");

		stmt.executeUpdate("INSERT INTO testrs VALUES (1)");
		stmt.executeUpdate("INSERT INTO testrs VALUES (2)");
		stmt.executeUpdate("INSERT INTO testrs VALUES (3)");
		stmt.executeUpdate("INSERT INTO testrs VALUES (4)");
		stmt.executeUpdate("INSERT INTO testrs VALUES (6)");
		stmt.executeUpdate("INSERT INTO testrs VALUES (9)");


                // Create the functions.
                stmt.execute ("CREATE OR REPLACE FUNCTION testspg__getRefcursor () RETURNS refcursor AS '"
                              + "declare v_resset; begin open v_resset for select id from testrs order by id; "
                              + "return v_resset; end;' LANGUAGE 'plpgsql';");
                stmt.execute ("CREATE OR REPLACE FUNCTION testspg__getEmptyRefcursor () RETURNS refcursor AS '"
                              + "declare v_resset; begin open v_resset for select id from testrs where id < 1 order by id; "
                              + "return v_resset; end;' LANGUAGE 'plpgsql';");
                stmt.close();
	}

	protected void tearDown() throws Exception
	{
                Statement stmt = con.createStatement ();
		stmt.execute ("drop FUNCTION testspg__getRefcursor ();");
		stmt.execute ("drop FUNCTION testspg__getEmptyRefcursor ();");
		TestUtil.dropTable(con, "testrs");
		TestUtil.closeDB(con);
	}

	public void testResult() throws Exception
	{
		CallableStatement call = con.prepareCall("{ ? = call testspg__getRefcursor () }");
		call.registerOutParameter(1, Types.OTHER);
		call.execute();
                ResultSet rs = (ResultSet) call.getObject(1);

                assertTrue(rs.next());
                assertTrue(rs.getInt(1) == 1);

                assertTrue(rs.next());
                assertTrue(rs.getInt(1) == 2);

                assertTrue(rs.next());
                assertTrue(rs.getInt(1) == 3);

                assertTrue(rs.next());
                assertTrue(rs.getInt(1) == 4);

                assertTrue(rs.next());
                assertTrue(rs.getInt(1) == 6);

                assertTrue(rs.next());
                assertTrue(rs.getInt(1) == 9);

                assertTrue(!rs.next());
                
		call.close();
	}


        public void testEmptyResult() throws Exception
	{
                CallableStatement call = con.prepareCall("{ ? = call testspg__getRefcursor () }");
                call.registerOutParameter(1, Types.OTHER);
                call.execute();

                ResultSet rs = (ResultSet) call.getObject(1);
                assertTrue(!rs.next());

                call.close();
	}
}
