# doc/Makefile — build the LaTeX user manuals into PDF.
#
# Requires a TeX Live with lualatex + luatexja (ltjsarticle) and the
# Menlo font (a macOS system font; substitute any monospaced font with
# box-drawing glyphs on other platforms).
#
#   make            # build both PDFs (EN + JA)
#   make en         # USERS_MANUAL_EN.pdf
#   make ja         # USERS_MANUAL_JA.pdf
#   make clean      # remove aux files (keeps the PDFs)
#   make distclean  # also remove the PDFs

LATEX = lualatex -interaction=nonstopmode -halt-on-error
AUX   = *.aux *.log *.toc *.out

.PHONY: all en ja clean distclean
all: en ja

en: USERS_MANUAL_EN.pdf
ja: USERS_MANUAL_JA.pdf

# Run twice so the table of contents resolves.
USERS_MANUAL_%.pdf: USERS_MANUAL_%.tex
	$(LATEX) $<
	$(LATEX) $<
	@rm -f $(AUX)

clean:
	rm -f $(AUX)

distclean: clean
	rm -f USERS_MANUAL_EN.pdf USERS_MANUAL_JA.pdf
