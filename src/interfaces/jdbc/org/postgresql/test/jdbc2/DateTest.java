package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * $Id: DateTest.java,v 1.1 2001/02/13 16:39:05 peter Exp $
 *
 * Some simple tests based on problems reported by users. Hopefully these will
 * help prevent previous problems from re-occuring ;-)
 *
 */
public class DateTest extends TestCase {

  public DateTest(String name) {
    super(name);
  }

  /**
   * Tests the time methods in ResultSet
   */
  public void testGetDate() {
    try {
      Connection con = JDBC2Tests.openDB();

      Statement st=con.createStatement();

      JDBC2Tests.createTable(con,"dt date");

      st.executeUpdate(JDBC2Tests.insert("'1950-02-07'"));
      st.executeUpdate(JDBC2Tests.insert("'1970-06-02'"));
      st.executeUpdate(JDBC2Tests.insert("'1999-08-11'"));
      st.executeUpdate(JDBC2Tests.insert("'2001-02-13'"));

      // Fall through helper
      checkTimeTest(con,st);

      st.close();

      JDBC2Tests.closeDB(con);
    } catch(Exception ex) {
      assert(ex.getMessage(),false);
    }
  }

  /**
   * Tests the time methods in PreparedStatement
   */
  public void testSetDate() {
    try {
      Connection con = JDBC2Tests.openDB();

      Statement st=con.createStatement();

      JDBC2Tests.createTable(con,"dt date");

      PreparedStatement ps = con.prepareStatement(JDBC2Tests.insert("?"));

      ps.setDate(1,getDate(1950,2,7));
      assert(!ps.execute()); // false as its an update!

      ps.setDate(1,getDate(1970,6,2));
      assert(!ps.execute()); // false as its an update!

      ps.setDate(1,getDate(1999,8,11));
      assert(!ps.execute()); // false as its an update!

      ps.setDate(1,getDate(2001,2,13));
      assert(!ps.execute()); // false as its an update!

      // Fall through helper
      checkTimeTest(con,st);

      ps.close();
      st.close();

      JDBC2Tests.closeDB(con);
    } catch(Exception ex) {
      assert(ex.getMessage(),false);
    }
  }

  /**
   * Helper for the TimeTests. It tests what should be in the db
   */
  private void checkTimeTest(Connection con,Statement st) throws SQLException {
    ResultSet rs=null;
    java.sql.Date t=null;

    rs=st.executeQuery(JDBC2Tests.select("dt"));
    assert(rs!=null);

    assert(rs.next());
    t = rs.getDate(1);
    assert(t!=null);
    assert(t.equals(getDate(1950,2,7)));

    assert(rs.next());
    t = rs.getDate(1);
    assert(t!=null);
    assert(t.equals(getDate(1970,6,2)));

    assert(rs.next());
    t = rs.getDate(1);
    assert(t!=null);
    assert(t.equals(getDate(1999,8,11)));

    assert(rs.next());
    t = rs.getDate(1);
    assert(t!=null);
    assert(t.equals(getDate(2001,2,13)));

    assert(!rs.next());

    rs.close();
  }

  /**
   * Yes this is ugly, but it gets the test done ;-)
   */
  private java.sql.Date getDate(int y,int m,int d) {
    return java.sql.Date.valueOf(JDBC2Tests.fix(y,4)+"-"+JDBC2Tests.fix(m,2)+"-"+JDBC2Tests.fix(d,2));
  }

}
