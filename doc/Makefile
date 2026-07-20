# doc/Makefile — build the LaTeX user manuals into PDF.
#
# Requires a TeX Live with lualatex + luatexja (ltjsarticle) and the
# Menlo font (a macOS system font; substitute any monospaced font with
# box-drawing glyphs on other platforms).
#
#   make            # build every PDF (manuals EN + JA, assessment)
#   make en         # USERS_MANUAL_EN.pdf
#   make ja         # USERS_MANUAL_JA.pdf
#   make assessment # OS_ASSESSMENT_JA.pdf  (docs/OS_ASSESSMENT_JA.md の清書版)
#   make clean      # remove aux files (keeps the PDFs)
#   make distclean  # also remove the PDFs

LATEX = lualatex -interaction=nonstopmode -halt-on-error
AUX   = *.aux *.log *.toc *.out
PDFS  = USERS_MANUAL_EN.pdf USERS_MANUAL_JA.pdf OS_ASSESSMENT_JA.pdf

.PHONY: all en ja assessment clean distclean
all: en ja assessment

en:         USERS_MANUAL_EN.pdf
ja:         USERS_MANUAL_JA.pdf
assessment: OS_ASSESSMENT_JA.pdf

# Run twice so the table of contents resolves.
USERS_MANUAL_%.pdf: USERS_MANUAL_%.tex
	$(LATEX) $<
	$(LATEX) $<
	@rm -f $(AUX)

OS_ASSESSMENT_JA.pdf: OS_ASSESSMENT_JA.tex
	$(LATEX) $<
	$(LATEX) $<
	@rm -f $(AUX)

clean:
	rm -f $(AUX)

distclean: clean
	rm -f $(PDFS)
