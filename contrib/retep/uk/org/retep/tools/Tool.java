package uk.org.retep.tools;

import javax.swing.JMenuBar;

/**
 * Tools can implement this interface to provide the parent manager (the big
 * application or the StandaloneApp class) enough details to display them.
 *
 * If a tool does not implement this class, it gets basic treatment.
 *
 * @author
 * @version 1.0
 */

public interface Tool
{
  /**
   * @return the JMenuBar for this tool, null if none.
   */
  public JMenuBar getMenuBar();

  /**
   * @return the title string to go into the JFrame/JInternalFrame's title bar.
   */
  public String getTitle();

  /**
   * Called by StandaloneApp to indicate this is within a StandaloneApp.
   * You should assume you are not in standalone mode until this is called.
   */
  public void setStandaloneMode(boolean aMode);

}