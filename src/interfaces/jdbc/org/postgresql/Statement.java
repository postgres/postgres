package org.postgresql;

import java.sql.SQLException;

/**
 * This class defines methods implemented by the two subclasses
 * org.postgresql.jdbc1.Statement and org.postgresql.jdbc2.Statement that are
 * unique to PostgreSQL's JDBC driver.
 *
 * <p>They are defined so that client code can cast to org.postgresql.Statement
 * without having to predetermine the jdbc driver type.
 *
 * <p>ie: Before this class existed, you had to use:
 *
 * <p>((org.postgresql.jdbc2.Statement)stat).getInsertedOID();
 *
 * <p>now you use:
 *
 * <p>((org.postgresql.Statement)stat).getInsertedOID();
 *
 * <p>As you can see, this is independent of JDBC1.2, JDBC2.0 or the upcoming
 * JDBC3.
 */

public abstract class Statement {

  public Statement() {
  }

  /**
    * Returns the status message from the current Result.<p>
    * This is used internally by the driver.
    *
    * @return status message from backend
    */
  public abstract String getResultStatusString();

  /**
   * @return the OID of the last row inserted
   */
  public abstract int getInsertedOID() throws SQLException;
}