package uk.org.retep.util;

import uk.org.retep.tools.Tool;
import uk.org.retep.util.Globals;
import uk.org.retep.util.ExceptionDialog;

import java.awt.*;
import javax.swing.*;
import java.awt.event.*;

/**
 * This provides the basic services needed for enabling some of the tools to
 * run in a Stand-alone fassion.
 *
 * Note: Because it's designed for standalone use, if this window is closed,
 * the JVM is terminated. Do not use for normal application use.
 *
 * $Id: StandaloneApp.java,v 1.1 2001/03/05 09:15:36 peter Exp $
 *
 * @author
 * @version 1.0
 */

public abstract class StandaloneApp extends JFrame
{
  public StandaloneApp(String[] aArgs)
  throws Exception
  {
    super(); // Initialise JFrame

    // Allow dialogs to work with us
    ExceptionDialog.setFrame(this);

    // Add a window listener
    this.addWindowListener(new java.awt.event.WindowAdapter()
    {
      public void windowClosing(WindowEvent e)
      {
        System.exit(0);
      }
    });

    // Parse the command line arguments
    Globals.getInstance().parseArguments(aArgs);

    // Now initialise this tool (init is overidden)
    JComponent tool = init();

    // Now add to this frame
    this.getContentPane().add(tool, BorderLayout.CENTER);

    // Finally call the Tool interface
    if(tool instanceof Tool) {
      Tool t = (Tool) tool;

      // Notify the tool we are a standalone
      t.setStandaloneMode(true);

      // Fetch the title
      setTitle(t.getTitle());

      // and a MenuBar (if needed)
      JMenuBar mb = t.getMenuBar();
      if(mb!=null) {
        setJMenuBar(t.getMenuBar());
      }
    } else {
      // Ok, set a default title string
      setTitle("RetepTools Standalone");
    }

  }

  /**
   * You must overide this method with your initialiser.
   */
  public abstract JComponent init() throws Exception;

}