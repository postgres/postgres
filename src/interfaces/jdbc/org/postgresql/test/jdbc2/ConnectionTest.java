package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * TestCase to test the internal functionality of org.postgresql.jdbc2.Connection
 * and it's superclass.
 *
 * PS: Do you know how difficult it is to type on a train? ;-)
 *
 * $Id: ConnectionTest.java,v 1.1 2001/02/07 09:13:20 peter Exp $
 */

public class ConnectionTest extends TestCase {

  /**
   * Constructor
   */
  public ConnectionTest(String name) {
    super(name);
  }

  /**
   * Tests the two forms of createStatement()
   */
  public void testCreateStatement() {
    try {
      java.sql.Connection conn = JDBC2Tests.openDB();

      // A standard Statement
      java.sql.Statement stat = conn.createStatement();
      assert(stat!=null);
      stat.close();

      // Ask for Updateable ResultSets
      stat = conn.createStatement(java.sql.ResultSet.TYPE_SCROLL_INSENSITIVE,java.sql.ResultSet.CONCUR_UPDATABLE);
      assert(stat!=null);
      stat.close();

    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  /**
   * Tests the two forms of prepareStatement()
   */
  public void testPrepareStatement() {
    try {
      java.sql.Connection conn = JDBC2Tests.openDB();

      String sql = "select source,cost,imageid from test_c";

      // A standard Statement
      java.sql.PreparedStatement stat = conn.prepareStatement(sql);
      assert(stat!=null);
      stat.close();

      // Ask for Updateable ResultSets
      stat = conn.prepareStatement(sql,java.sql.ResultSet.TYPE_SCROLL_INSENSITIVE,java.sql.ResultSet.CONCUR_UPDATABLE);
      assert(stat!=null);
      stat.close();

    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  /**
   * Put the test for createPrepareCall here
   */
  public void testPrepareCall() {
  }

  /**
   * Test nativeSQL
   */
  public void testNativeSQL() {
    // For now do nothing as it returns itself
  }

  /**
   * Test autoCommit (both get & set)
   */
  public void testTransactions() {
    try {
      java.sql.Connection con = JDBC2Tests.openDB();
      java.sql.Statement st;
      java.sql.ResultSet rs;

      // Turn it off
      con.setAutoCommit(false);
      assert(!con.getAutoCommit());

      // Turn it back on
      con.setAutoCommit(true);
      assert(con.getAutoCommit());

      // Now test commit
      st = con.createStatement();
      st.executeUpdate("insert into test_a (imagename,image,id) values ('comttest',1234,5678)");

      con.setAutoCommit(false);

      // Now update image to 9876 and commit
      st.executeUpdate("update test_a set image=9876 where id=5678");
      con.commit();
      rs = st.executeQuery("select image from test_a where id=5678");
      assert(rs.next());
      assert(rs.getInt(1)==9876);
      rs.close();

      // Now try to change it but rollback
      st.executeUpdate("update test_a set image=1111 where id=5678");
      con.rollback();
      rs = st.executeQuery("select image from test_a where id=5678");
      assert(rs.next());
      assert(rs.getInt(1)==9876); // Should not change!
      rs.close();

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  /**
   * Simple test to see if isClosed works
   */
  public void testIsClosed() {
    try {
      Connection con = JDBC2Tests.openDB();

      // Should not say closed
      assert(!con.isClosed());

      JDBC2Tests.closeDB(con);

      // Should now say closed
      assert(con.isClosed());

    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

}