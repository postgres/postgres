<!-- $Header: /cvsroot/pgsql/doc/src/sgml/stylesheet.dsl,v 1.12 2001/09/30 16:05:54 petere Exp $ -->
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
(element type ($mono-seq$))

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

(define %graphic-default-extension%
  (cond (tex-backend "eps")
        (rtf-backend "ai"))) ;; ApplixWare?

]]> <!-- %output-print -->

<![ %output-text; [
;; customize HTML stylesheet to be suitable for dumping plain text
;; (for INSTALL file)

(define %section-autolabel% #f)
(define %chapter-autolabel% #f)
(define $generate-chapter-toc$ (lambda () #f))

]]> <!-- %output-text -->

  </style-specification-body>
 </style-specification>

 <external-specification id="docbook" document="dbstyle">
</style-sheet>
