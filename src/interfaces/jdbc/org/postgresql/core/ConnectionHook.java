package org.postgresql.core;

import java.sql.SQLException;
import java.util.ArrayList;
import java.util.Iterator;
import org.postgresql.Connection;

/**
 * ConnectionHook keeps track of all open Connections. It's used only in
 * Java2 (JDK1.3+) VM's, and it's purpose is to close all connections cleanly
 * when the VM terminates.
 *
 * Important: This only works for JDK1.3 or later as it uses methods new to
 * that JDK.
 *
 * This is a singleton object ;-)
 *
 * How it works: This is an initiated but un-started Thread. When it's created,
 * it registers it'self with the Runtime.addShutdownHook() method.
 *
 * When a Connection is made, two static methods in org.postgresql.Driver are
 * called. For pre JDK1.3 these are noops, but for 1.3+ ANT adds calls to
 * methods in this class, which add/remove it from an ArrayList.
 *
 * Now when the VM terminates it starts this thread, which then Itterates
 * through the ArrayList and closes each Connection.
 *
 * Obviously this doesn't trap things like Runtime.halt() or SIGKILL etc, but
 * this captures 99% of all other forms of VM termination.
 *
 */

public class ConnectionHook implements Runnable
{
  /**
   * This ensures that the hook is created and the system is notified of it.
   *
   * Important: We have to use an instance, as we have to pass a reference to
   * the VM.
   */
  private static final ConnectionHook SINGLETON = new ConnectionHook();

  /**
   * The currently open connections
   */
  private ArrayList cons = new ArrayList();

  /**
   * Constructor. This is private because we are a singleton. Here we set
   * our selves up, and then register with the VM.
   */
  private ConnectionHook() {
    super();
    Runtime.getRuntime().addShutdownHook(new Thread(this));
  }

  /**
   * Called by Driver, this simply forces us to be created.
   */
  public static final void init() {
  }

  /**
   * This is used by org.postgresql.Connection to register itself. Because it's
   * called internally, we don't bother with checking to see if it's already
   * present (performance boost).
   */
  public static final void open(Connection con) {
    SINGLETON.cons.add(con);
  }

  /**
   * This is used by org.postgresql.Connection to remove itself.
   */
  public static final void close(Connection con) {
    SINGLETON.cons.remove(con);
  }

  /**
   * This is called by the VM when it terminates. It itterates through the list
   * of connections and implicitly closes them.
   */
  public void run() {
    Iterator i = cons.iterator();
    while(i.hasNext()) {
      Connection c = (Connection) i.next();
      try {
        c.close();
      } catch(SQLException e) {
        // Ignore as at this point we are dying anyhow ;-)
      }
    }
  }

}
