<!-- $Header: /cvsroot/pgsql/doc/src/sgml/stylesheet.dsl,v 1.14 2001/10/09 18:46:00 petere Exp $ -->
<!DOCTYPE style-sheet PUBLIC "-//James Clark//DTD DSSSL Style Sheet//EN" [

<!-- must turn on one of these with -i on the jade command line -->
<!ENTITY % output-html          "IGNORE">
<!ENTITY % output-print         "IGNORE">
<!ENTITY % output-text          "IGNORE">

<![ %output-html; [
<!ENTITY dbstyle PUBLIC "-//Norman Walsh//DOCUMENT DocBook HTML Stylesheet//EN" CDATA DSSSL>
]]>

<![ %output-print; [
<!ENTITY dbstyle PUBLIC "-//Norman Walsh//DOCUMENT DocBook Print Stylesheet//EN" CDATA DSSSL>
]]>

<![ %output-text; [
<!ENTITY dbstyle PUBLIC "-//Norman Walsh//DOCUMENT DocBook HTML Stylesheet//EN" CDATA DSSSL>
]]>

]>

<style-sheet>
 <style-specification use="docbook">
  <style-specification-body> 

(define pgsql-docs-list "pgsql-docs@postgresql.org")

(define %refentry-xref-manvolnum% #f)
(define %callout-graphics% #f)
(define %show-comments% #f)

(define %content-title-end-punct% 
  '(#\. #\! #\? #\:))

(element lineannotation ($italic-seq$))
(element structfield ($mono-seq$))
(element structname ($mono-seq$))
(element symbol ($mono-seq$))
(element type ($mono-seq$))


;; The rules in the default stylesheet for productname format it as
;; a paragraph.  This may be suitable for productname directly
;; within *info, but it's nonsense when productname is used
;; inline, as we do.
(mode set-titlepage-recto-mode
  (element (para productname) ($charseq$)))
(mode set-titlepage-verso-mode
  (element (para productname) ($charseq$)))
(mode book-titlepage-recto-mode
  (element (para productname) ($charseq$)))
(mode book-titlepage-verso-mode
  (element (para productname) ($charseq$)))
;; Add more here if needed...


<![ %output-html; [
;; customize the html stylesheet

(define %section-autolabel% #t)
(define %generate-legalnotice-link% #t)
(define %html-ext%              ".html")
(define %root-filename%         "index")
(define %link-mailto-url%       (string-append "mailto:" pgsql-docs-list))
(define %use-id-as-filename%    #t)
(define %stylesheet%            "stylesheet.css")
(define %graphic-default-extension% "gif")

;; Returns the depth of auto TOC that should be made at the nd-level
(define (toc-depth nd)
  (cond ((string=? (gi nd) (normalize "book")) 3)
	((string=? (gi nd) (normalize "set")) 2)
	((string=? (gi nd) (normalize "part")) 2)
	((string=? (gi nd) (normalize "chapter")) 2)
	(else 1)))

;; Put a horizontal line in the set TOC
(define (set-titlepage-separator side)
  (if (equal? side 'recto)
      (make empty-element gi: "HR")
      (empty-sosofo)))

;; Put date of creation into header
(define %html-header-tags% 
  (list (list "META" '("NAME" "creation") (list "CONTENT" (time->string (time) #t)))))

(define html-index #t)

;; Block elements are allowed in PARA in DocBook, but not in P in
;; HTML.  With %fix-para-wrappers% turned on, the stylesheets attempt
;; to avoid putting block elements in HTML P tags by outputting
;; additional end/begin P pairs around them.
(define %fix-para-wrappers% #t)

;; ...but we need to do some extra work to make the above apply to PRE
;; as well.  (mostly pasted from dbverb.dsl)
(define ($verbatim-display$ indent line-numbers?)
  (let ((content (make element gi: "PRE"
                       attributes: (list
                                    (list "CLASS" (gi)))
                       (if (or indent line-numbers?)
                           ($verbatim-line-by-line$ indent line-numbers?)
                           (process-children)))))
    (if %shade-verbatim%
        (make element gi: "TABLE"
              attributes: ($shade-verbatim-attr$)
              (make element gi: "TR"
                    (make element gi: "TD"
                          content)))
	(make sequence
	  (para-check)
	  content
	  (para-check 'restart)))))

;; ...and for notes.
(element note
  (make sequence
    (para-check)
    ($admonition$)
    (para-check 'restart)))

;;; XXX The above is very ugly.  It might be better to run 'tidy' on
;;; the resulting *.html files.

]]> <!-- %output-html -->

<![ %output-print; [
;; customize the print stylesheet

(define %section-autolabel%     #t)
(define %default-quadding%      'justify)
(define bop-footnotes           #t)
(define %hyphenation%
  (if tex-backend #t #f))
(define %refentry-new-page%     #t)
(define %refentry-keep%         #f)

(define %graphic-default-extension%
  (cond (tex-backend "eps")
        (rtf-backend "ai"))) ;; ApplixWare?

(define %footnote-ulinks%
  (and tex-backend
       (>= (string->number "1.73") 1.73)))

;; Format legalnotice justified and with space between paragraphs.
(mode book-titlepage-verso-mode
  (element (legalnotice para)
    (make paragraph
      use: book-titlepage-verso-style	;; alter this if ever it needs to appear elsewhere
      quadding: %default-quadding%
      line-spacing: (* 0.8 (inherited-line-spacing))
      font-size: (* 0.8 (inherited-font-size))
      space-before: (* 0.8 %para-sep%)
      space-after: (* 0.8 %para-sep%)
      (process-children))))


;; Fix spacing bug in variablelists
(define (process-listitem-content)
  (if (absolute-first-sibling?)
      (make sequence
        (process-children-trim))
      (next-match)))


;; Default stylesheets format simplelists are tables.  This just
;; spells trouble for Jade.

(define %simplelist-indent% 1em)

(define (my-simplelist-vert members)
  (make display-group
    space-before: %para-sep%
    space-after: %para-sep%
    start-indent: (+ %simplelist-indent% (inherited-start-indent))
    (process-children)))

(element simplelist
  (let ((type (attribute-string (normalize "type")))
        (cols (if (attribute-string (normalize "columns"))
                  (if (> (string->number (attribute-string (normalize "columns"))) 0)
                      (string->number (attribute-string (normalize "columns")))
                      1)
                  1))
        (members (select-elements (children (current-node)) (normalize "member"))))
    (cond
       ((equal? type (normalize "inline"))
	(if (equal? (gi (parent (current-node)))
		    (normalize "para"))
	    (process-children)
	    (make paragraph
	      space-before: %para-sep%
	      space-after: %para-sep%
	      start-indent: (inherited-start-indent))))
       ((equal? type (normalize "vert"))
        (my-simplelist-vert members))
       ((equal? type (normalize "horiz"))
        (simplelist-table 'row    cols members)))))
 
(element member
  (let ((type (inherited-attribute-string (normalize "type"))))
    (cond
     ((equal? type (normalize "inline"))
      (make sequence
	(process-children)
	(if (not (last-sibling?))
	    (literal ", ")
	    (literal ""))))
      ((equal? type (normalize "vert"))
       (make paragraph
	 space-before: 0pt
	 space-after: 0pt))
      ((equal? type (normalize "horiz"))
       (make paragraph
	 quadding: 'start
	 (process-children))))))

]]> <!-- %output-print -->

<![ %output-text; [
;; customize HTML stylesheet to be suitable for dumping plain text
;; (for INSTALL file)

(define %section-autolabel% #f)
(define %chapter-autolabel% #f)
(define $generate-chapter-toc$ (lambda () #f))

;; For text output, produce "ASCII markup" for emphasis and such.

(define ($asterix-seq$ #!optional (sosofo (process-children)))
  (make sequence
    (literal "*")
    sosofo
    (literal "*")))
 
(define ($dquote-seq$ #!optional (sosofo (process-children)))
  (make sequence
    (literal (gentext-start-quote))
    sosofo
    (literal (gentext-end-quote))))
 
(element (para command) ($dquote-seq$))
(element (para emphasis) ($asterix-seq$))
(element (para filename) ($dquote-seq$))
(element (para option) ($dquote-seq$))
(element (para replaceable) ($dquote-seq$))
(element (para userinput) ($dquote-seq$))

]]> <!-- %output-text -->

  </style-specification-body>
 </style-specification>

 <external-specification id="docbook" document="dbstyle">
</style-sheet>
