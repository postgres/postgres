package org.postgresql.test;

import junit.framework.TestSuite;
import junit.framework.TestCase;

import org.postgresql.test.jdbc2.*;
import java.sql.*;

/**
 * Executes all known tests for JDBC2 and includes some utility methods.
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
   * Helper - creates a test table for use by a test
   */
  public static void createTable(Connection conn,String columns) {
    try {
      Statement st = conn.createStatement();

      // Ignore the drop
      try {
        st.executeUpdate("drop table "+getTableName());
      } catch(SQLException se) {
      }

      // Now create the table
      st.executeUpdate("create table "+getTableName()+" ("+columns+")");

      st.close();
    } catch(SQLException ex) {
      TestCase.assert(ex.getMessage(),false);
    }
  }

  /**
   * Variant used when more than one table is required
   */
  public static void createTable(Connection conn,String id,String columns) {
    try {
      Statement st = conn.createStatement();

      // Ignore the drop
      try {
        st.executeUpdate("drop table "+getTableName(id));
      } catch(SQLException se) {
      }

      // Now create the table
      st.executeUpdate("create table "+getTableName(id)+" ("+columns+")");

      st.close();
    } catch(SQLException ex) {
      TestCase.assert(ex.getMessage(),false);
    }
  }

  /**
   * Helper - generates INSERT SQL - very simple
   */
  public static String insert(String values) {
    return insert(null,values);
  }
  public static String insert(String columns,String values) {
    String s = "INSERT INTO "+getTableName();
    if(columns!=null)
      s=s+" ("+columns+")";
    return s+" VALUES ("+values+")";
  }

  /**
   * Helper - generates SELECT SQL - very simple
   */
  public static String select(String columns) {
    return select(columns,null,null);
  }
  public static String select(String columns,String where) {
    return select(columns,where,null);
  }
  public static String select(String columns,String where,String other) {
    String s = "SELECT "+columns+" FROM "+getTableName();
    if(where!=null)
      s=s+" WHERE "+where;
    if(other!=null)
      s=s+" "+other;
    return s;
  }

  /**
   * Helper - returns the test table's name
   * This is defined by the tablename property. If not defined it defaults to
   * jdbctest
   */
  public static String getTableName() {
    if(tablename==null)
      tablename=System.getProperty("tablename","jdbctest");
    return tablename;
  }

  /**
   * As getTableName() but the id is a suffix. Used when more than one table is
   * required in a test.
   */
  public static String getTableName(String id) {
    if(tablename==null)
      tablename=System.getProperty("tablename","jdbctest");
    return tablename+"_"+id;
  }

  /**
   * Cache used by getTableName() [its used a lot!]
   */
  private static String tablename;

  /**
   * Helper to prefix a number with leading zeros - ugly but it works...
   * @param v value to prefix
   * @param l number of digits (0-10)
   */
  public static String fix(int v,int l) {
    String s = "0000000000".substring(0,l)+Integer.toString(v);
    return s.substring(s.length()-l);
  }

  /**
   * Number of milliseconds in a day
   */
  public static final long DAYMILLIS = 24*3600*1000;

  /**
   * The main entry point for JUnit
   */
  public static TestSuite suite() {
    TestSuite suite= new TestSuite();

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

    // Connectivity/Protocols

    // ResultSet
    suite.addTestSuite(DateTest.class);
    suite.addTestSuite(TimeTest.class);
    suite.addTestSuite(TimestampTest.class);

    // PreparedStatement

    // MetaData

    // Fastpath/LargeObject

    // Other misc tests, based on previous problems users have had or specific
    // features some applications require.
    suite.addTestSuite(JBuilderTest.class);
    suite.addTestSuite(MiscTest.class);

    // That's all folks
    return suite;
  }
}
