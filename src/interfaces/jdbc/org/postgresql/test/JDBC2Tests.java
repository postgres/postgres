package org.postgresql.test;

import junit.framework.TestSuite;
import junit.framework.TestCase;

import org.postgresql.test.jdbc2.*;
import java.sql.*;

/**
 * Executes all known tests for JDBC2
 */
public class JDBC2Tests extends TestSuite {
  /**
   * Returns the Test database JDBC URL
   */
  public static String getURL() {
    return System.getProperty("database");
  }

  /**
   * Returns the Postgresql username
   */
  public static String getUser() {
    return System.getProperty("username");
  }

  /**
   * Returns the user's password
   */
  public static String getPassword() {
    return System.getProperty("password");
  }

  /**
   * helper - opens a connection. Static so other classes can call it.
   */
  public static java.sql.Connection openDB() {
    try {
      Class.forName("org.postgresql.Driver");
      return java.sql.DriverManager.getConnection(JDBC2Tests.getURL(),JDBC2Tests.getUser(),JDBC2Tests.getPassword());
    } catch(ClassNotFoundException ex) {
      TestCase.assert(ex.getMessage(),false);
    } catch(SQLException ex) {
      TestCase.assert(ex.getMessage(),false);
    }
    return null;
  }

  /**
   * Helper - closes an open connection. This rewrites SQLException to a failed
   * assertion. It's static so other classes can use it.
   */
  public static void closeDB(Connection conn) {
    try {
      if(conn!=null)
        conn.close();
    } catch(SQLException ex) {
      TestCase.assert(ex.getMessage(),false);
    }
  }

  /**
   * The main entry point for JUnit
   */
  public static TestSuite suite() {
    TestSuite suite= new TestSuite();

    //
    // Add one line per class in our test cases. These should be in order of
    // complexity.
    //
    // ie: ANTTest should be first as it ensures that test parameters are
    // being sent to the suite.
    //

    // Basic Driver internals
    suite.addTestSuite(ANTTest.class);
    suite.addTestSuite(DriverTest.class);
    suite.addTestSuite(ConnectionTest.class);

    // Connectivity/Protocols

    // ResultSet

    // PreparedStatement

    // MetaData

    // Fastpath/LargeObject

    // That's all folks
    return suite;
  }
}