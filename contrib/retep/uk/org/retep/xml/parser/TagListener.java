package uk.org.retep.xml.parser;

import java.util.HashMap;
import java.io.CharArrayWriter;

/**
 * This interface defines the methods a class needs to implement if it wants the
 * xml parser to notify it of any xml tags.
 */

public interface TagListener {
  /**
   * This is called when a tag has just been started.
   * <p><b>NB:</b> args is volatile, so if you use it beyond the lifetime of
   * this call, then you must make a copy of the HashMap (and not use simply
   * store this HashMap).
   * @param level The number of tags above this
   * @param tag The tag name
   * @param args A HashMap of any arguments
   */
  public void tagStart(int level,String tag,HashMap args);
  /**
   * This method is called by ContHandler to process a tag once it has been
   * fully processed.
   * <p><b>NB:</b> content is volatile, so you must copy its contents if you use
   * it beyond the lifetime of this call.
   * @param content CharArrayWriter containing the content of the tag.
   */
  public void tagContent(CharArrayWriter content);
}