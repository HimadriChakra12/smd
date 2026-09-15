FAQ
====

Q: Why isn't my inline `**bold *and italic* together**` rendering right?
A: The inline parser is a single flat pass - it doesn't nest emphasis.
   See TODO.md. Use one or the other in a given span for now.

Q: Math isn't rendering, I just see the raw latex source.
A: smd needs pdflatex and pdftoppm on your PATH. It degrades to
   showing the source instead of erroring out if they're missing.
   Check `which pdflatex pdftoppm`.

Q: Can it fetch remote images (http:// in markdown)?
A: No, on purpose. That would mean a network stack again, which is
   most of what we just cut out by dropping webkit.

Q: The window doesn't update when I save from vim/some editors.
A: It watches the containing directory (not the file's inode
   directly), specifically so editors that save-by-rename still
   trigger a reload. If it still doesn't, check that inotify limits
   on your system aren't exhausted (`cat /proc/sys/fs/inotify/max_user_watches`).

Q: Why GTK instead of something even more minimal like a raw X11 + xcb setup?
A: cairo+pango need a font/rendering stack either way, and GTK gives
   us that plus scrolling/window management essentially for free. The
   heavy dependency being cut here was webkit, not GTK itself.
