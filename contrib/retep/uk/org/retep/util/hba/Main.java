package uk.org.retep.util.hba;

import uk.org.retep.util.ExceptionDialog;
import uk.org.retep.util.Globals;
import uk.org.retep.util.Logger;
import uk.org.retep.util.StandaloneApp;

import java.io.IOException;
import javax.swing.JComponent;
import javax.swing.JPanel;

/**
 * Standalone entry point for the Properties editor
 *
 * $Id: Main.java,v 1.1 2001/03/05 09:15:37 peter Exp $
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

    Editor editor = new Editor();

    if(globals.getArgumentCount()>0) {
      editor.openFile(globals.getArgument(0));
    }

    return editor;
  }

  public static void main(String[] args)
  throws Exception
  {
    Main main = new Main(args);
    main.pack();
    main.setVisible(true);
  }
}
