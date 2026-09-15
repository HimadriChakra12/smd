todo
-----
- nested inline emphasis (**bold with *italic* inside**)
- nested lists (indentation-based)
- tables
- search-in-document (/ to search, n/N to jump)
- proper line-height metrics for inline images/math instead of the
  1.15x-of-body-font guess
- syntax highlighting in fenced code blocks (currently just monospace,
  no colorization - would need a small tokenizer per language, or
  shell out to something like chroma/pygments, which cuts against
  minimalism, so: undecided)
- reference-style links ([text][ref] + [ref]: url)
- footnotes
- html: at least respect inline style="color:...;font-weight:..." even
  without a real CSS cascade

not planned (deliberately out of scope)
------------------------------------------
- real CSS support in html mode
- javascript
- remote image/network fetching (would violate the whole point of
  dropping webkit - no network stack, no attack surface from it)
- WYSIWYG editing - smd is a viewer, not an editor
