package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * $Id: TimestampTest.java,v 1.2 2001/02/16 16:45:01 peter Exp $
 *
 * This has been the most controversial pair of methods since 6.5 was released!
 *
 * From now on, any changes made to either getTimestamp or setTimestamp
 * MUST PASS this TestCase!!!
 *
 */
public class TimestampTest extends TestCase {

  public TimestampTest(String name) {
    super(name);
  }

  /**
   * Tests the time methods in ResultSet
   */
  public void testGetTimestamp() {
    try {
      Connection con = JDBC2Tests.openDB();

      Statement st=con.createStatement();

      JDBC2Tests.createTable(con,"ts timestamp");

      st.executeUpdate(JDBC2Tests.insert("'1950-02-07 15:00:00'"));

      // Before you ask why 8:13:00 and not 7:13:00, this is a problem with the
      // getTimestamp method in this TestCase. It's simple, brain-dead. It
      // simply doesn't know about summer time. As this date is in June, it's
      // summer (GMT wise).
      //
      // This case needs some work done on it.
      //
      st.executeUpdate(JDBC2Tests.insert("'"+getTimestamp(1970,6,2,8,13,0).toString()+"'"));

      //st.executeUpdate(JDBC2Tests.insert("'1950-02-07'"));

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
  public void testSetTimestamp() {
    try {
      Connection con = JDBC2Tests.openDB();

      Statement st=con.createStatement();

      JDBC2Tests.createTable(con,"ts timestamp");

      PreparedStatement ps = con.prepareStatement(JDBC2Tests.insert("?"));

      ps.setTimestamp(1,getTimestamp(1950,2,7,15,0,0));
      assert(!ps.execute()); // false as its an update!

      // Before you ask why 8:13:00 and not 7:13:00, this is a problem with the
      // getTimestamp method in this TestCase. It's simple, brain-dead. It
      // simply doesn't know about summer time. As this date is in June, it's
      // summer (GMT wise).
      //
      // This case needs some work done on it.
      //
      ps.setTimestamp(1,getTimestamp(1970,6,2,7,13,0));
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
    java.sql.Timestamp t=null;

    rs=st.executeQuery(JDBC2Tests.select("ts"));
    assert(rs!=null);

    assert(rs.next());
    t = rs.getTimestamp(1);
    assert(t!=null);
    assert(t.equals(getTimestamp(1950,2,7,15,0,0)));

    assert(rs.next());
    t = rs.getTimestamp(1);
    assert(t!=null);

    // Seems Daylight saving is ignored?
    assert(t.equals(getTimestamp(1970,6,2,8,13,0)));

    assert(!rs.next()); // end of table. Fail if more entries exist.

    rs.close();
  }

  /**
   * These implement depreciated methods in java.sql.Time
   */
  private static final long dayms = 24*3600*1000;

  /**
   * Yes this is ugly, but it gets the test done ;-)
   *
   * Actually its buggy. We need a better solution to this, then the hack of adding 1 hour to
   * entries in June above don't need setting.
   */
  private java.sql.Timestamp getTimestamp(int y,int m,int d,int h,int mn,int se) {
    return java.sql.Timestamp.valueOf(JDBC2Tests.fix(y,4)+"-"+JDBC2Tests.fix(m,2)+"-"+JDBC2Tests.fix(d,2)+" "+JDBC2Tests.fix(h,2)+":"+JDBC2Tests.fix(mn,2)+":"+JDBC2Tests.fix(se,2)+"."+JDBC2Tests.fix(0,9));
  }

}
