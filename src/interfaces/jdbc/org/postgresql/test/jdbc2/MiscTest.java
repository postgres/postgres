package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * $Id: MiscTest.java,v 1.2 2001/09/23 04:11:14 momjian Exp $
 *
 * Some simple tests based on problems reported by users. Hopefully these will
 * help prevent previous problems from re-occuring ;-)
 *
 */
public class MiscTest extends TestCase {

  public MiscTest(String name) {
    super(name);
  }

  /**
   * Some versions of the driver would return rs as a null?
   *
   * Sasha <ber0806@iperbole.bologna.it> was having this problem.
   *
   * Added Feb 13 2001
   */
  public void testDatabaseSelectNullBug() {
    try {
      Connection con = JDBC2Tests.openDB();

      Statement st=con.createStatement();
      ResultSet rs=st.executeQuery("select datname from pg_database");
      assertNotNull(rs);

      while(rs.next()){
	String s = rs.getString(1);
      }

      rs.close();
      st.close();

      JDBC2Tests.closeDB(con);
    } catch(Exception ex) {
      fail(ex.getMessage());
    }
  }
}
