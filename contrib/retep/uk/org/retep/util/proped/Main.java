package uk.org.retep.util.proped;

import uk.org.retep.util.ExceptionDialog;
import uk.org.retep.util.Globals;
import uk.org.retep.util.Logger;
import uk.org.retep.util.StandaloneApp;

import java.io.IOException;
import java.util.Iterator;
import javax.swing.JComponent;

/**
 * Standalone entry point for the Properties editor
 *
 * $Id: Main.java,v 1.1 2001/03/05 09:15:38 peter Exp $
 */

public class Main extends StandaloneApp
{
  public Main(String[] args)
  throws Exception
  {
    super(args);
  }

  public JComponent init()
  throws Exception
  {
    Globals globals = Globals.getInstance();

    PropertyEditor panel = new PropertyEditor();

    // Only handle 1 open at a time in standalone mode
    if(globals.getArgumentCount()>0) {
      try {
        panel.openFile(globals.getArgument(0));
      } catch(IOException ioe) {
        ExceptionDialog.displayException(ioe,"while loading "+globals.getArgument(0));
        throw (Exception) ioe.fillInStackTrace();
      }
    }

    return panel;
  }

  public static void main(String[] args)
  throws Exception
  {
    Main main = new Main(args);
    main.pack();
    main.setVisible(true);
  }
}