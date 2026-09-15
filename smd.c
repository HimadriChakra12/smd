/* smd - a small markdown (and barely-html) viewer
 * was surf, a webkit browser, before someone got tired of webkit.
 * see LICENSE for copyright details. */

#include <ctype.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <pango/pangocairo.h>

#define LENGTH(x) (sizeof(x) / sizeof((x)[0]))

#include "arg.h"
char *argv0;

/* --- document model ------------------------------------------------ */

enum {
	BLK_PARA, BLK_HEAD, BLK_CODE, BLK_QUOTE,
	BLK_HR, BLK_ITEM, BLK_IMAGE, BLK_MATH,
	BLK_TABLE, BLK_CELL,
};

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

/* one clickable region inside a block's layout */
typedef struct {
	int start, end; /* byte range in the block's plain text */
	char *href;
} Link;

/* one inline image/formula embedded in a block's flowing text */
typedef struct {
	int start;      /* byte offset of its placeholder char in Block->text */
	GdkPixbuf *pb;
} InlineImg;

typedef struct Block {
	int type;
	int level;         /* heading level, 1-6 */
	int ordered;        /* list item: numbered? */
	int num;              /* list item: its number */
	int header;            /* BLK_CELL: is this a header-row cell? */
	char *raw;              /* source text, pre inline-parsing */
	char *alt, *src;         /* image alt + path/url */

	GString *text;             /* built lazily: plain text w/ placeholders */
	PangoAttrList *attrs;
	Link *links;
	int nlinks;
	InlineImg *inlineimgs;      /* inline images/formulas, kept alive here */
	int ninlineimgs;

	PangoLayout *layout;           /* cached, rebuilt when width/zoom changes */
	GdkPixbuf *pixbuf;               /* for BLK_IMAGE / BLK_MATH */
	int builtwidth;
	int y, h;                          /* computed during reflow */

	/* BLK_TABLE only: a grid of BLK_CELL blocks, row-major, plus the
	 * per-column geometry reflow() works out */
	struct Block **cells;
	int ncols, nrows;
	int *colw;
	int *colalign;
	int *rowh, *rowy;

	struct Block *next;
} Block;

static Block *doc, *doctail;
static int dochh; /* total document height, px */

/* --- app state ------------------------------------------------------ */

static GtkWidget *win, *scroller, *canvas;
static GtkAdjustment *vadj;
static char *filepath, *basedir, *cachedir;
static int ishtml;
static double zoomfactor = 1.0;
static guint reloadsrc;
static int inotifyfd = -1, watchwd = -1;
static GHashTable *mathcache; /* tex source -> GdkPixbuf* */

/* Arg/Key live here (not in config.h) so config.h can just be data +
 * a table of key->function bindings, dwm-style */
typedef union {
	int i;
	double f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	unsigned int key;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

static void quit(const Arg *arg);
static void reload(const Arg *arg);
static void scroll(const Arg *arg);
static void page(const Arg *arg);
static void totop(const Arg *arg);
static void tobottom(const Arg *arg);
static void zoom(const Arg *arg);
static void zoomreset(const Arg *arg);

#include "config.h"

/* --- small helpers ---------------------------------------------------- */

static void die(const char *fmt, ...) __attribute__((noreturn));

static void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void *
ecalloc(size_t n, size_t sz)
{
	void *p = calloc(n, sz);
	if (!p)
		die("smd: out of memory");
	return p;
}

/* fork+exec, don't wait around (double-fork so we don't leave zombies) */
static void
spawn(char *const argv[])
{
	if (fork() == 0) {
		if (fork() == 0) {
			setsid();
			execvp(argv[0], argv);
			_exit(0);
		}
		_exit(0);
	}
	wait(NULL);
}

/* fork+exec and actually wait for it, stdout/stderr silenced */
static int
runcmd(char *const argv[])
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) {
			dup2(null, 1);
			dup2(null, 2);
			close(null);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static unsigned long
hashstr(const char *s)
{
	unsigned long h = 1469598103934665603UL; /* fnv1a, nothing fancy */
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211UL;
	}
	return h;
}

static char *
resolvepath(const char *ref)
{
	char *p;
	if (!ref)
		return NULL;
	if (ref[0] == '/' || strstr(ref, "://"))
		return g_strdup(ref);
	p = g_build_filename(basedir, ref, NULL);
	return p;
}

/* --- block list plumbing --------------------------------------------- */

static Block *
newblock(int type)
{
	Block *b = ecalloc(1, sizeof *b);
	b->type = type;
	if (doctail)
		doctail->next = b;
	else
		doc = b;
	doctail = b;
	return b;
}

static void
freeblock(Block *b)
{
	free(b->raw);
	free(b->alt);
	free(b->src);
	if (b->text)
		g_string_free(b->text, TRUE);
	if (b->attrs)
		pango_attr_list_unref(b->attrs);
	if (b->layout)
		g_object_unref(b->layout);
	if (b->pixbuf)
		g_object_unref(b->pixbuf);
	for (int i = 0; i < b->ninlineimgs; i++)
		g_object_unref(b->inlineimgs[i].pb);
	free(b->inlineimgs);
	for (int i = 0; i < b->nlinks; i++)
		free(b->links[i].href);
	free(b->links);
	if (b->cells) {
		for (int i = 0; i < b->ncols * b->nrows; i++)
			if (b->cells[i])
				freeblock(b->cells[i]);
		free(b->cells);
	}
	free(b->colw);
	free(b->colalign);
	free(b->rowh);
	free(b->rowy);
	free(b);
}

static void
freedoc(void)
{
	Block *b, *n;
	for (b = doc; b; b = n) {
		n = b->next;
		freeblock(b);
	}
	doc = doctail = NULL;
}

/* --- latex --------------------------------------------------------- */

static GdkPixbuf *
latexrender(const char *tex, int display)
{
	char key[4100], base[PATH_MAX], texpath[PATH_MAX], pdfpath[PATH_MAX],
	     pngpath[PATH_MAX], dpistr[16], outdiropt[PATH_MAX + 20];
	gpointer hit;
	FILE *f;
	GdkPixbuf *pb;

	snprintf(key, sizeof key, "%d:%s", display, tex);
	if (!mathcache)
		mathcache = g_hash_table_new_full(g_str_hash, g_str_equal, free, g_object_unref);
	if ((hit = g_hash_table_lookup(mathcache, key)))
		return GDK_PIXBUF(hit);

	snprintf(base, sizeof base, "%s/%016lx", cachedir, hashstr(key));
	snprintf(pngpath, sizeof pngpath, "%s.png", base);

	if (access(pngpath, F_OK) != 0) {
		snprintf(texpath, sizeof texpath, "%s.tex", base);
		snprintf(pdfpath, sizeof pdfpath, "%s.pdf", base);

		if (!(f = fopen(texpath, "w")))
			return NULL;
		fputs("\\documentclass[preview,border=1pt,varwidth]{standalone}\n"
		      "\\usepackage{amsmath,amssymb}\n"
		      "\\begin{document}\n", f);
		fputs(display ? "\\[" : "$", f);
		fputs(tex, f);
		fputs(display ? "\\]" : "$", f);
		fputs("\n\\end{document}\n", f);
		fclose(f);

		snprintf(outdiropt, sizeof outdiropt, "-output-directory=%s", cachedir);
		char *texargv[] = { "pdflatex", "-interaction=nonstopmode",
			"-halt-on-error", outdiropt, texpath, NULL };
		if (runcmd(texargv) != 0 || access(pdfpath, F_OK) != 0)
			return NULL;

		snprintf(dpistr, sizeof dpistr, "%d", mathdpi);
		char *ppmargv[] = { "pdftoppm", "-png", "-r", dpistr, pdfpath, base, NULL };
		runcmd(ppmargv);

		/* single-page pdf -> "<base>-1.png" */
		char produced[PATH_MAX];
		snprintf(produced, sizeof produced, "%s-1.png", base);
		if (access(produced, F_OK) == 0)
			rename(produced, pngpath);

		unlink(texpath);
		snprintf(texpath, sizeof texpath, "%s.log", base); unlink(texpath);
		snprintf(texpath, sizeof texpath, "%s.aux", base); unlink(texpath);
		unlink(pdfpath);
	}

	pb = gdk_pixbuf_new_from_file(pngpath, NULL);
	if (pb)
		g_hash_table_insert(mathcache, g_strdup(key), g_object_ref(pb));
	return pb;
}

/* --- inline (span-level) markdown ------------------------------------ */

enum { RUN_TEXT, RUN_BOLD, RUN_ITALIC, RUN_CODE, RUN_LINK, RUN_IMG, RUN_MATH };

typedef struct {
	int type;
	char *text; /* display text / alt / latex source */
	char *href; /* link target / image src, NULL otherwise */
} Run;

/* pull the next run out of s, return pointer past it (or NULL at end) */
static const char *
nextrun(const char *s, Run *r)
{
	const char *close;
	int bang = 0;

	memset(r, 0, sizeof *r);

	if (*s == '!' && s[1] == '[')
		bang = 1;
	if ((bang && s[1] == '[') || (!bang && *s == '[')) {
		const char *p = s + (bang ? 2 : 1);
		const char *lend = strchr(p, ']');
		if (lend && lend[1] == '(' && (close = strchr(lend, ')'))) {
			r->type = bang ? RUN_IMG : RUN_LINK;
			r->text = g_strndup(p, lend - p);
			r->href = g_strndup(lend + 2, close - (lend + 2));
			return close + 1;
		}
	}
	if (s[0] == '*' && s[1] == '*' && (close = strstr(s + 2, "**")) && close != s + 2) {
		r->type = RUN_BOLD;
		r->text = g_strndup(s + 2, close - (s + 2));
		return close + 2;
	}
	if (s[0] == '_' && s[1] == '_' && (close = strstr(s + 2, "__")) && close != s + 2) {
		r->type = RUN_BOLD;
		r->text = g_strndup(s + 2, close - (s + 2));
		return close + 2;
	}
	if (s[0] == '`' && (close = strchr(s + 1, '`')) && close != s + 1) {
		r->type = RUN_CODE;
		r->text = g_strndup(s + 1, close - (s + 1));
		return close + 1;
	}
	if (s[0] == '$' && (close = strchr(s + 1, '$')) && close != s + 1) {
		r->type = RUN_MATH;
		r->text = g_strndup(s + 1, close - (s + 1));
		return close + 1;
	}
	if (s[0] == '*' && s[1] != ' ' && (close = strchr(s + 1, '*')) && close != s + 1) {
		r->type = RUN_ITALIC;
		r->text = g_strndup(s + 1, close - (s + 1));
		return close + 1;
	}
	if (s[0] == '_' && s[1] != ' ' && (close = strchr(s + 1, '_')) && close != s + 1) {
		r->type = RUN_ITALIC;
		r->text = g_strndup(s + 1, close - (s + 1));
		return close + 1;
	}

	/* plain run: everything up to the next thing that might be special */
	{
		const char *p = s + 1;
		while (*p && !strchr("*_`$[!", *p))
			p++;
		/* be a little careful: a lone '!' not followed by '[' is plain */
		while (*p == '!' && p[1] != '[')
			p++;
		r->type = RUN_TEXT;
		r->text = g_strndup(s, p - s);
		return p;
	}
}

static double
fontpx(const char *desc)
{
	PangoFontDescription *d = pango_font_description_from_string(desc);
	double px = pango_font_description_get_size(d) / (double)PANGO_SCALE;
	if (!pango_font_description_get_size_is_absolute(d))
		px *= 96.0 / 72.0;
	pango_font_description_free(d);
	return px;
}

/* turn a run of markdown source text into b->text/attrs/links/pixbufs */
static void
buildspan(Block *b, const char *src, double basepx)
{
	GString *out = g_string_new(NULL);
	PangoAttrList *al = pango_attr_list_new();
	InlineImg *imgs = NULL;
	int nimgs = 0;
	Link *links = NULL;
	int nlinks = 0;
	const char *p = src;

	while (*p) {
		Run r;
		int start;
		const char *next = nextrun(p, &r);

		start = out->len;
		if (r.type == RUN_IMG || r.type == RUN_MATH) {
			GdkPixbuf *pb = NULL, *scaled;
			int pw, ph, th, tw;

			if (r.type == RUN_IMG) {
				char *path = resolvepath(r.href);
				if (path && !strstr(path, "://"))
					pb = gdk_pixbuf_new_from_file(path, NULL);
				g_free(path);
			} else {
				/* borrowed from mathcache - do not unref it below */
				pb = latexrender(r.text, 0);
			}

			if (pb) {
				pw = gdk_pixbuf_get_width(pb);
				ph = gdk_pixbuf_get_height(pb);
				th = (int)(basepx * 1.15);
				tw = MAX(1, pw * th / MAX(ph, 1));
				scaled = gdk_pixbuf_scale_simple(pb, tw, th, GDK_INTERP_BILINEAR);
				if (r.type == RUN_IMG)
					g_object_unref(pb); /* we own this one, scaled is our copy now */
				g_string_append_len(out, "\xef\xbf\xbc", 3); /* U+FFFC */

				/* shape attr reserves the right amount of line space;
				 * we paint the actual pixbuf ourselves in drawblock()
				 * since pango doesn't hand the shape renderer a
				 * usefully positioned cairo_t in this pango version */
				PangoRectangle ink = { 0, -th * PANGO_SCALE, tw * PANGO_SCALE, th * PANGO_SCALE };
				PangoAttribute *sh = pango_attr_shape_new_with_data(&ink, &ink, scaled, NULL, NULL);
				sh->start_index = start;
				sh->end_index = out->len;
				pango_attr_list_insert(al, sh);

				imgs = realloc(imgs, (nimgs + 1) * sizeof *imgs);
				imgs[nimgs].start = start;
				imgs[nimgs].pb = scaled;
				nimgs++;
			} else {
				/* couldn't load/render it: fall back to showing the source */
				char *fallback = r.type == RUN_IMG
					? g_strdup_printf("[image: %s]", r.text)
					: g_strdup_printf("$%s$", r.text);
				g_string_append(out, fallback);
				PangoAttribute *fam = pango_attr_family_new("monospace");
				fam->start_index = start; fam->end_index = out->len;
				pango_attr_list_insert(al, fam);
				g_free(fallback);
			}
		} else {
			g_string_append(out, r.text);
			int end = out->len;
			switch (r.type) {
			case RUN_BOLD:
				{
					PangoAttribute *a = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
					a->start_index = start; a->end_index = end;
					pango_attr_list_insert(al, a);
				}
				break;
			case RUN_ITALIC:
				{
					PangoAttribute *a = pango_attr_style_new(PANGO_STYLE_ITALIC);
					a->start_index = start; a->end_index = end;
					pango_attr_list_insert(al, a);
				}
				break;
			case RUN_CODE:
				{
					GdkRGBA fg, bgc;
					gdk_rgba_parse(&fg, colcode);
					gdk_rgba_parse(&bgc, colcodebg);
					PangoAttribute *fam = pango_attr_family_new("monospace");
					PangoAttribute *fc = pango_attr_foreground_new(fg.red * 65535, fg.green * 65535, fg.blue * 65535);
					PangoAttribute *bg = pango_attr_background_new(bgc.red * 65535, bgc.green * 65535, bgc.blue * 65535);
					fam->start_index = fc->start_index = bg->start_index = start;
					fam->end_index = fc->end_index = bg->end_index = end;
					pango_attr_list_insert(al, fam);
					pango_attr_list_insert(al, fc);
					pango_attr_list_insert(al, bg);
				}
				break;
			case RUN_LINK:
				{
					GdkRGBA fg;
					gdk_rgba_parse(&fg, collink);
					PangoAttribute *fc = pango_attr_foreground_new(fg.red * 65535, fg.green * 65535, fg.blue * 65535);
					PangoAttribute *u = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
					fc->start_index = u->start_index = start;
					fc->end_index = u->end_index = end;
					pango_attr_list_insert(al, fc);
					pango_attr_list_insert(al, u);
					links = realloc(links, (nlinks + 1) * sizeof *links);
					links[nlinks].start = start;
					links[nlinks].end = end;
					links[nlinks].href = g_strdup(r.href);
					nlinks++;
				}
				break;
			}
		}
		g_free(r.text);
		g_free(r.href);
		p = next;
	}

	b->text = out;
	b->attrs = al;
	b->inlineimgs = imgs;
	b->ninlineimgs = nimgs;
	b->links = links;
	b->nlinks = nlinks;
}

/* --- markdown block parser -------------------------------------------- */

static char *
striplead(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

/* split src into lines in place (','\n' -> '\0'), keeping true blank
 * lines as empty strings instead of collapsing them like strtok would -
 * table lookahead needs to peek the next line without losing that */
static char **
splitlines(char *src, int *n)
{
	int cap = 64, cnt = 0;
	char **arr = ecalloc(cap, sizeof *arr);
	char *p = src;

	arr[cnt++] = p;
	for (; *p; p++) {
		if (*p == '\n') {
			*p = '\0';
			if (cnt == cap) {
				cap *= 2;
				arr = realloc(arr, cap * sizeof *arr);
			}
			arr[cnt++] = p + 1;
		}
	}
	for (int i = 0; i < cnt; i++) {
		size_t l = strlen(arr[i]);
		if (l && arr[i][l - 1] == '\r')
			arr[i][l - 1] = '\0';
	}
	*n = cnt;
	return arr;
}

/* "| a | b |" -> {"a","b"}. handles \| escapes and a missing lead/trail pipe */
static char **
splitcells(const char *line, int *ncells)
{
	char *s = g_strdup(line);
	g_strstrip(s);
	char *start = s[0] == '|' ? s + 1 : s;
	size_t l = strlen(start);
	if (l && start[l - 1] == '|' && (l < 2 || start[l - 2] != '\\'))
		start[l - 1] = '\0';

	GPtrArray *cells = g_ptr_array_new();
	GString *cur = g_string_new(NULL);
	for (const char *p = start; *p; p++) {
		if (*p == '\\' && p[1]) {
			g_string_append_c(cur, p[1]);
			p++;
			continue;
		}
		if (*p == '|') {
			char *c = g_strdup(cur->str);
			g_strstrip(c);
			g_ptr_array_add(cells, c);
			g_string_set_size(cur, 0);
			continue;
		}
		g_string_append_c(cur, *p);
	}
	{
		char *c = g_strdup(cur->str);
		g_strstrip(c);
		g_ptr_array_add(cells, c);
	}
	g_string_free(cur, TRUE);
	g_free(s);

	*ncells = cells->len;
	char **arr = ecalloc(cells->len, sizeof(char *));
	for (guint i = 0; i < cells->len; i++)
		arr[i] = g_ptr_array_index(cells, i);
	g_ptr_array_free(cells, FALSE);
	return arr;
}

static void
freecells(char **cells, int n)
{
	for (int i = 0; i < n; i++)
		g_free(cells[i]);
	free(cells);
}

static int
istablesep(const char *s)
{
	int sawdash = 0;
	for (; *s; s++) {
		if (*s == ' ' || *s == '\t' || *s == '|' || *s == ':')
			continue;
		if (*s == '-') {
			sawdash = 1;
			continue;
		}
		return 0;
	}
	return sawdash;
}

static Block *
newcell(const char *raw, int header)
{
	Block *c = ecalloc(1, sizeof *c);
	c->type = BLK_CELL;
	c->header = header;
	c->raw = strdup(raw ? raw : "");
	return c;
}

/* consumes the header+separator+data rows starting at lines[i], returns
 * the index of the last line it ate (caller's for-loop continues after) */
static int
parsetable(char **lines, int nlines, int i)
{
	int ncols, nseps, j;
	char **hdr = splitcells(striplead(lines[i]), &ncols);
	char **seps = splitcells(striplead(lines[i + 1]), &nseps);
	int *align = ecalloc(ncols, sizeof *align);

	for (int c = 0; c < ncols; c++) {
		const char *cell = c < nseps ? seps[c] : "-";
		int n = strlen(cell);
		int left = n && cell[0] == ':';
		int right = n && cell[n - 1] == ':';
		align[c] = (left && right) ? ALIGN_CENTER : right ? ALIGN_RIGHT : ALIGN_LEFT;
	}
	freecells(seps, nseps);

	/* how many consecutive non-blank lines follow as data rows? */
	for (j = i + 2; j < nlines && *striplead(lines[j]); j++)
		;
	int ndata = j - (i + 2);

	Block *t = newblock(BLK_TABLE);
	t->ncols = ncols;
	t->nrows = ndata + 1;
	t->colalign = align;
	t->cells = ecalloc(ncols * t->nrows, sizeof(Block *));

	for (int c = 0; c < ncols; c++)
		t->cells[c] = newcell(hdr[c], 1);
	freecells(hdr, ncols);

	for (int r = 0; r < ndata; r++) {
		int nc;
		char **row = splitcells(striplead(lines[i + 2 + r]), &nc);
		for (int c = 0; c < ncols; c++)
			t->cells[(r + 1) * ncols + c] = newcell(c < nc ? row[c] : "", 0);
		freecells(row, nc);
	}
	return j - 1;
}

static int
ishr(const char *s)
{
	int n = 0;
	char c = 0;
	for (; *s; s++) {
		if (*s == ' ')
			continue;
		if (!c)
			c = *s;
		if (*s != c || (c != '-' && c != '*' && c != '_'))
			return 0;
		n++;
	}
	return n >= 3;
}

static void
parsemd(char *src)
{
	char **lines;
	int nlines;
	GString *para = NULL, *quote = NULL, *code = NULL, *math = NULL;
	int infence = 0, inmath = 0;
	char fencelang[64] = "";

#define FLUSHPARA() do { if (para) { newblock(BLK_PARA)->raw = g_string_free(para, FALSE); para = NULL; } } while (0)
#define FLUSHQUOTE() do { if (quote) { newblock(BLK_QUOTE)->raw = g_string_free(quote, FALSE); quote = NULL; } } while (0)

	lines = splitlines(src, &nlines);
	for (int li = 0; li < nlines; li++) {
		char *line = lines[li];
		char *t;

		if (infence) {
			t = striplead(line);
			if (!strncmp(t, "```", 3) || !strncmp(t, "~~~", 3)) {
				Block *b = newblock(BLK_CODE);
				b->raw = g_string_free(code, FALSE);
				b->alt = strdup(fencelang);
				code = NULL;
				infence = 0;
				continue;
			}
			if (code->len)
				g_string_append_c(code, '\n');
			g_string_append(code, line);
			continue;
		}
		if (inmath) {
			t = striplead(line);
			if (!strcmp(t, "$$")) {
				newblock(BLK_MATH)->raw = g_string_free(math, FALSE);
				math = NULL;
				inmath = 0;
				continue;
			}
			if (math->len)
				g_string_append_c(math, '\n');
			g_string_append(math, line);
			continue;
		}

		t = striplead(line);

		if (!*t) {
			FLUSHPARA();
			FLUSHQUOTE();
			continue;
		}
		if (!strncmp(t, "```", 3) || !strncmp(t, "~~~", 3)) {
			FLUSHPARA(); FLUSHQUOTE();
			snprintf(fencelang, sizeof fencelang, "%s", t + 3);
			code = g_string_new(NULL);
			infence = 1;
			continue;
		}
		if (!strcmp(t, "$$")) {
			FLUSHPARA(); FLUSHQUOTE();
			math = g_string_new(NULL);
			inmath = 1;
			continue;
		}
		if (t[0] == '$' && t[1] == '$' && strlen(t) > 4 && !strncmp(t + strlen(t) - 2, "$$", 2)) {
			FLUSHPARA(); FLUSHQUOTE();
			newblock(BLK_MATH)->raw = g_strndup(t + 2, strlen(t) - 4);
			continue;
		}
		if (t[0] == '#') {
			int lvl = 0;
			while (t[lvl] == '#' && lvl < 6)
				lvl++;
			if (t[lvl] == ' ') {
				FLUSHPARA(); FLUSHQUOTE();
				Block *b = newblock(BLK_HEAD);
				b->level = lvl;
				b->raw = strdup(striplead(t + lvl));
				continue;
			}
		}
		if (ishr(t)) {
			FLUSHPARA(); FLUSHQUOTE();
			newblock(BLK_HR);
			continue;
		}
		if (t[0] == '>' ) {
			FLUSHPARA();
			if (!quote)
				quote = g_string_new(NULL);
			else
				g_string_append_c(quote, ' ');
			g_string_append(quote, striplead(t + 1));
			continue;
		}
		if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
			FLUSHPARA(); FLUSHQUOTE();
			Block *b = newblock(BLK_ITEM);
			b->raw = strdup(striplead(t + 1));
			continue;
		}
		if (isdigit((unsigned char)t[0])) {
			char *q = t;
			while (isdigit((unsigned char)*q))
				q++;
			if (q[0] == '.' && q[1] == ' ') {
				FLUSHPARA(); FLUSHQUOTE();
				Block *b = newblock(BLK_ITEM);
				b->ordered = 1;
				b->num = atoi(t);
				b->raw = strdup(striplead(q + 1));
				continue;
			}
		}
		if (t[0] == '!' && t[1] == '[') {
			char *lend = strchr(t, ']');
			if (lend && lend[1] == '(') {
				char *close = strchr(lend, ')');
				if (close && !close[1]) {
					FLUSHPARA(); FLUSHQUOTE();
					Block *b = newblock(BLK_IMAGE);
					b->alt = g_strndup(t + 2, lend - (t + 2));
					b->src = g_strndup(lend + 2, close - (lend + 2));
					continue;
				}
			}
		}

		if (strchr(t, '|') && li + 1 < nlines && istablesep(striplead(lines[li + 1]))) {
			FLUSHPARA(); FLUSHQUOTE();
			li = parsetable(lines, nlines, li);
			continue;
		}

		FLUSHQUOTE();
		if (!para)
			para = g_string_new(NULL);
		else
			g_string_append_c(para, ' ');
		g_string_append(para, t);
	}
	FLUSHPARA();
	FLUSHQUOTE();
	if (code)
		newblock(BLK_CODE)->raw = g_string_free(code, FALSE);
	if (math)
		newblock(BLK_MATH)->raw = g_string_free(math, FALSE);
	free(lines);

#undef FLUSHPARA
#undef FLUSHQUOTE
}

/* --- bare-bones html -------------------------------------------------- */

/* decode the handful of entities anyone actually uses */
static char *
unescapehtml(const char *s)
{
	GString *o = g_string_new(NULL);
	while (*s) {
		if (!strncmp(s, "&amp;", 5)) { g_string_append_c(o, '&'); s += 5; }
		else if (!strncmp(s, "&lt;", 4)) { g_string_append_c(o, '<'); s += 4; }
		else if (!strncmp(s, "&gt;", 4)) { g_string_append_c(o, '>'); s += 4; }
		else if (!strncmp(s, "&quot;", 6)) { g_string_append_c(o, '"'); s += 6; }
		else if (!strncmp(s, "&#39;", 5) || !strncmp(s, "&apos;", 6)) {
			g_string_append_c(o, '\''); s += s[3] == '3' ? 5 : 6;
		} else if (!strncmp(s, "&nbsp;", 6)) { g_string_append_c(o, ' '); s += 6; }
		else { g_string_append_c(o, *s); s++; }
	}
	return g_string_free(o, FALSE);
}

/* map a couple of inline html tags onto our markdown run syntax so we
 * can reuse buildspan() instead of writing a second inline parser */
static char *
htmltomdinline(const char *s)
{
	GString *o = g_string_new(NULL);
	while (*s) {
		if (*s == '<') {
			const char *end = strchr(s, '>');
			if (!end) { g_string_append_c(o, *s++); continue; }
			int taglen = end - s - 1;
			char tag[32] = "";
			snprintf(tag, sizeof tag, "%.*s", MIN(taglen, 31), s + 1);
			if (!strcasecmp(tag, "b") || !strcasecmp(tag, "strong")) g_string_append(o, "**");
			else if (!strcasecmp(tag, "/b") || !strcasecmp(tag, "/strong")) g_string_append(o, "**");
			else if (!strcasecmp(tag, "i") || !strcasecmp(tag, "em")) g_string_append(o, "*");
			else if (!strcasecmp(tag, "/i") || !strcasecmp(tag, "/em")) g_string_append(o, "*");
			else if (!strcasecmp(tag, "code")) g_string_append(o, "`");
			else if (!strcasecmp(tag, "/code")) g_string_append(o, "`");
			else if (!strncasecmp(tag, "a ", 2)) {
				char *h = strstr(s, "href=");
				char q = 0;
				GString *href = g_string_new(NULL);
				if (h && h < end) {
					h += 5;
					q = *h == '"' || *h == '\'' ? *h++ : 0;
					while (h < end && *h != q && (q || (*h != ' ' && *h != '>')))
						g_string_append_c(href, *h++);
				}
				g_string_append_c(o, '[');
				g_string_append(o, "\x01"); /* marker: link text starts, closed below */
				g_string_append(o, href->str);
				g_string_append(o, "\x02");
				g_string_free(href, TRUE);
			} else if (!strcasecmp(tag, "/a")) {
				/* nothing: handled via post-pass below */
			}
			s = end + 1;
			continue;
		}
		g_string_append_c(o, *s++);
	}
	{
		/* fix up the crude [\x01href\x02text -> [text](href) shuffle */
		char *raw = g_string_free(o, FALSE);
		GString *fin = g_string_new(NULL);
		for (char *p = raw; *p; p++) {
			if (*p == '[' && p[1] == '\x01') {
				char *hstart = p + 2;
				char *hend = strchr(hstart, '\x02');
				char *tstart = hend + 1;
				/* link text runs until the next tag/marker - good enough
				 * for the "just to see it render" html mode this is */
				char *scan = tstart;
				while (*scan && *scan != '[')
					scan++;
				g_string_append_c(fin, '[');
				g_string_append_len(fin, tstart, scan - tstart);
				g_string_append_c(fin, ']');
				g_string_append_c(fin, '(');
				g_string_append_len(fin, hstart, hend - hstart);
				g_string_append_c(fin, ')');
				p = scan - 1;
				continue;
			}
			g_string_append_c(fin, *p);
		}
		free(raw);
		char *dec = unescapehtml(fin->str);
		g_string_free(fin, TRUE);
		return dec;
	}
}

static void
parsehtml(char *src)
{
	char *p = src, *bodystart;

	/* skip past doctype/head, we don't do css, this is "just to see" mode */
	if ((bodystart = strcasestr(p, "<body")))
		p = strchr(bodystart, '>') + 1;

	while (*p) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		if (*p != '<') {
			char *end = strchr(p, '<');
			if (!end)
				end = p + strlen(p);
			if (end > p) {
				char *chunk = g_strndup(p, end - p);
				Block *b = newblock(BLK_PARA);
				b->raw = htmltomdinline(chunk);
				free(chunk);
			}
			p = end;
			continue;
		}

		char *tagend = strchr(p, '>');
		if (!tagend)
			break;
		char tag[32] = "";
		{
			char *q = p + 1;
			int i = 0;
			while (q < tagend && *q != ' ' && *q != '/' && i < 31)
				tag[i++] = *q++;
			tag[i] = 0;
		}

		if (!strcasecmp(tag, "img")) {
			char *s = strstr(p, "src=");
			char *a = strstr(p, "alt=");
			Block *b = newblock(BLK_IMAGE);
			if (s && s < tagend) {
				s += 4; char q = *s == '"' ? *s++ : 0;
				char *e = q ? strchr(s, q) : strchr(s, ' ');
				b->src = g_strndup(s, (e && e < tagend ? e : tagend) - s);
			}
			if (a && a < tagend) {
				a += 4; char q = *a == '"' ? *a++ : 0;
				char *e = q ? strchr(a, q) : strchr(a, ' ');
				b->alt = g_strndup(a, (e && e < tagend ? e : tagend) - a);
			}
			p = tagend + 1;
			continue;
		}
		if (!strcasecmp(tag, "hr")) {
			newblock(BLK_HR);
			p = tagend + 1;
			continue;
		}
		if (!strcasecmp(tag, "br")) {
			p = tagend + 1;
			continue;
		}
		if (tag[0] == '!' || !strcasecmp(tag, "head") || !strcasecmp(tag, "script") ||
		    !strcasecmp(tag, "style")) {
			char close[40];
			snprintf(close, sizeof close, "</%s>", tag[0] == '!' ? "!--" : tag);
			char *e = strcasestr(p, tag[0] == '!' ? "-->" : close);
			p = e ? e + strlen(tag[0] == '!' ? "-->" : close) : tagend + 1;
			continue;
		}

		int h = !strcasecmp(tag, "h1") ? 1 : !strcasecmp(tag, "h2") ? 2 :
			!strcasecmp(tag, "h3") ? 3 : !strcasecmp(tag, "h4") ? 4 :
			!strcasecmp(tag, "h5") ? 5 : !strcasecmp(tag, "h6") ? 6 : 0;
		int isli = !strcasecmp(tag, "li");
		int ispre = !strcasecmp(tag, "pre");
		int isquote = !strcasecmp(tag, "blockquote");
		int ispara = !strcasecmp(tag, "p") || !strcasecmp(tag, "div") ||
			!strcasecmp(tag, "span") || h || isli || isquote;

		if (ispre) {
			char *cend = strcasestr(tagend, "</pre>");
			if (!cend) cend = tagend + strlen(tagend);
			char *raw = g_strndup(tagend + 1, cend - (tagend + 1));
			/* strip a single wrapping <code> if present, keep it simple */
			char *inner = raw;
			if (!strncasecmp(inner, "<code>", 6)) {
				inner += 6;
				char *iend = strcasestr(inner, "</code>");
				if (iend) *iend = 0;
			}
			Block *b = newblock(BLK_CODE);
			b->raw = unescapehtml(inner);
			b->alt = strdup("");
			free(raw);
			p = cend + 6;
			continue;
		}
		if (ispara) {
			char closetag[16];
			snprintf(closetag, sizeof closetag, "</%s>", tag);
			char *cend = strcasestr(tagend, closetag);
			if (!cend) cend = tagend + strlen(tagend);
			char *chunk = g_strndup(tagend + 1, cend - (tagend + 1));
			Block *b = newblock(h ? BLK_HEAD : isli ? BLK_ITEM : isquote ? BLK_QUOTE : BLK_PARA);
			b->level = h;
			b->raw = htmltomdinline(chunk);
			free(chunk);
			p = cend[0] ? cend + strlen(closetag) : cend;
			continue;
		}

		/* ul/ol/table/whatever: don't care about the wrapper, just recurse
		 * into its contents by stepping past the opening tag */
		p = tagend + 1;
	}
}

/* --- layout / drawing -------------------------------------------------- */

static const char *
blockfont(Block *b, char sizebuf[64])
{
	const char *base = b->type == BLK_HEAD ? fonthead[MAX(0, b->level - 1)] :
		b->type == BLK_CODE ? fontmono : fontbody;
	int bold = b->type == BLK_CELL && b->header;
	if (zoomfactor == 1.0 && !bold)
		return base;
	PangoFontDescription *d = pango_font_description_from_string(base);
	if (zoomfactor != 1.0) {
		int sz = pango_font_description_get_size(d);
		pango_font_description_set_size(d, (int)(sz * zoomfactor));
	}
	if (bold)
		pango_font_description_set_weight(d, PANGO_WEIGHT_BOLD);
	char *s = pango_font_description_to_string(d);
	snprintf(sizebuf, 64, "%s", s);
	g_free(s);
	pango_font_description_free(d);
	return sizebuf;
}

static int
blockindent(Block *b)
{
	if (b->type == BLK_ITEM)
		return listindent;
	if (b->type == BLK_QUOTE)
		return quoteindent;
	return 0;
}

static void
ensurelayout(Block *b, int width)
{
	if (b->layout && b->builtwidth == width)
		return;
	if (b->layout) {
		g_object_unref(b->layout);
		b->layout = NULL;
	}
	if (!b->text)
		buildspan(b, b->raw ? b->raw : "", fontpx(fontbody) * zoomfactor);

	char sizebuf[64];
	PangoFontDescription *fd = pango_font_description_from_string(blockfont(b, sizebuf));

	PangoLayout *l = gtk_widget_create_pango_layout(canvas, NULL);
	pango_layout_set_font_description(l, fd);
	pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_width(l, MAX(20, width - blockindent(b)) * PANGO_SCALE);
	if (b->type == BLK_CODE) {
		pango_layout_set_text(l, b->raw ? b->raw : "", -1);
	} else {
		pango_layout_set_text(l, b->text->str, b->text->len);
		pango_layout_set_attributes(l, b->attrs);
	}
	pango_font_description_free(fd);

	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	b->layout = l;
	b->builtwidth = width;
	b->h = h + (b->type == BLK_CODE ? codepad * 2 : 0);
}

static void
ensurepixbuf(Block *b, int width)
{
	if (b->pixbuf)
		return;
	GdkPixbuf *pb = NULL;
	int owned = 0;
	if (b->type == BLK_IMAGE) {
		char *path = resolvepath(b->src);
		if (path && !strstr(path, "://")) {
			pb = gdk_pixbuf_new_from_file(path, NULL);
			owned = 1;
		}
		g_free(path);
	} else {
		pb = latexrender(b->raw, 1); /* borrowed from mathcache */
	}
	if (!pb)
		return;
	int pw = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb);
	double scale = MIN(1.0, (double)width / pw) * zoomfactor;
	if (scale < 1.0 - 1e-6 || zoomfactor > 1.0 + 1e-6) {
		int nw = MAX(1, (int)(pw * scale)), nh = MAX(1, (int)(ph * scale));
		GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, nw, nh, GDK_INTERP_BILINEAR);
		if (owned)
			g_object_unref(pb);
		pb = sc;
		owned = 1;
	}
	/* block owns a ref either way now: take one if we're just borrowing */
	b->pixbuf = owned ? pb : g_object_ref(pb);
}

static void
reflowtable(Block *t, int width)
{
	int ncols = t->ncols, nrows = t->nrows;
	int *natural = ecalloc(ncols, sizeof *natural);

	/* measure pass: how wide would each column like to be, unwrapped */
	for (int r = 0; r < nrows; r++) {
		for (int c = 0; c < ncols; c++) {
			Block *cell = t->cells[r * ncols + c];
			if (!cell->text)
				buildspan(cell, cell->raw ? cell->raw : "", fontpx(fontbody) * zoomfactor);
			char sizebuf[64];
			PangoFontDescription *fd = pango_font_description_from_string(blockfont(cell, sizebuf));
			PangoLayout *tmp = gtk_widget_create_pango_layout(canvas, NULL);
			pango_layout_set_font_description(tmp, fd);
			pango_layout_set_text(tmp, cell->text->str, cell->text->len);
			pango_layout_set_attributes(tmp, cell->attrs);
			int w, h;
			pango_layout_get_pixel_size(tmp, &w, &h);
			g_object_unref(tmp);
			pango_font_description_free(fd);
			w += 2 * cellpad;
			if (w > width)
				w = width; /* one absurd cell shouldn't blow up the table */
			if (w > natural[c])
				natural[c] = w;
		}
	}

	int total = 0;
	for (int c = 0; c < ncols; c++)
		total += natural[c];

	free(t->colw);
	t->colw = ecalloc(ncols, sizeof *t->colw);
	if (total <= width) {
		for (int c = 0; c < ncols; c++)
			t->colw[c] = natural[c];
	} else {
		double scale = (double)width / total;
		for (int c = 0; c < ncols; c++)
			t->colw[c] = MAX(2 * cellpad + 20, (int)(natural[c] * scale));
	}

	free(t->rowh);
	free(t->rowy);
	t->rowh = ecalloc(nrows, sizeof *t->rowh);
	t->rowy = ecalloc(nrows, sizeof *t->rowy);

	int y = 0;
	for (int r = 0; r < nrows; r++) {
		int rowh = 0;
		for (int c = 0; c < ncols; c++) {
			Block *cell = t->cells[r * ncols + c];
			ensurelayout(cell, t->colw[c] - 2 * cellpad);
			pango_layout_set_alignment(cell->layout,
				t->colalign[c] == ALIGN_CENTER ? PANGO_ALIGN_CENTER :
				t->colalign[c] == ALIGN_RIGHT ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
			rowh = MAX(rowh, cell->h + 2 * cellpad);
		}
		t->rowy[r] = y;
		t->rowh[r] = rowh;
		y += rowh;
	}
	t->h = y + blockgap;
	free(natural);
}

static void
reflow(void)
{
	int width = gtk_widget_get_allocated_width(canvas) - 2 * margin;
	int y = margin;

	if (width < 40)
		width = 400; /* not allocated yet, guess something sane */

	for (Block *b = doc; b; b = b->next) {
		b->y = y;
		switch (b->type) {
		case BLK_HR:
			b->h = blockgap * 2 + hrthick;
			break;
		case BLK_IMAGE:
		case BLK_MATH:
			ensurepixbuf(b, width);
			b->h = (b->pixbuf ? gdk_pixbuf_get_height(b->pixbuf) : (int)(fontpx(fontbody) * 1.4)) + blockgap;
			break;
		case BLK_TABLE:
			reflowtable(b, width);
			break;
		default:
			ensurelayout(b, width);
			b->h += blockgap;
			break;
		}
		y += b->h;
	}
	dochh = y + margin;
	gtk_widget_set_size_request(canvas, -1, dochh);
}

static void
setrgba(cairo_t *cr, const char *hex)
{
	GdkRGBA c;
	gdk_rgba_parse(&c, hex);
	gdk_cairo_set_source_rgba(cr, &c);
}

static void
drawinlineimgs(cairo_t *cr, Block *b, int x, int y)
{
	for (int i = 0; i < b->ninlineimgs; i++) {
		PangoRectangle rect;
		GdkPixbuf *pb = b->inlineimgs[i].pb;
		pango_layout_index_to_pos(b->layout, b->inlineimgs[i].start, &rect);
		cairo_save(cr);
		cairo_translate(cr, x + rect.x / (double)PANGO_SCALE, y + rect.y / (double)PANGO_SCALE);
		gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
	}
}

static void
drawblock(cairo_t *cr, Block *b)
{
	int x = margin + blockindent(b);

	switch (b->type) {
	case BLK_HR:
		setrgba(cr, colbar);
		cairo_rectangle(cr, margin, b->y + blockgap, gtk_widget_get_allocated_width(canvas) - 2 * margin, hrthick);
		cairo_fill(cr);
		return;
	case BLK_IMAGE:
		if (!b->pixbuf) {
			setrgba(cr, colquote);
			cairo_move_to(cr, x, b->y + 12);
			PangoLayout *l = gtk_widget_create_pango_layout(canvas, NULL);
			char msg[300];
			snprintf(msg, sizeof msg, "[image unavailable: %s]", b->alt ? b->alt : b->src);
			pango_layout_set_text(l, msg, -1);
			pango_cairo_show_layout(cr, l);
			g_object_unref(l);
			return;
		}
		cairo_save(cr);
		cairo_translate(cr, x, b->y);
		gdk_cairo_set_source_pixbuf(cr, b->pixbuf, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		return;
	case BLK_MATH:
		if (!b->pixbuf) {
			setrgba(cr, colquote);
			PangoLayout *l = gtk_widget_create_pango_layout(canvas, NULL);
			PangoFontDescription *fd = pango_font_description_from_string(fontmono);
			pango_layout_set_font_description(l, fd);
			pango_layout_set_text(l, b->raw, -1);
			cairo_move_to(cr, x, b->y);
			pango_cairo_show_layout(cr, l);
			pango_font_description_free(fd);
			g_object_unref(l);
			return;
		}
		cairo_save(cr);
		cairo_translate(cr, margin + MAX(0, (gtk_widget_get_allocated_width(canvas) - 2 * margin - gdk_pixbuf_get_width(b->pixbuf)) / 2), b->y);
		gdk_cairo_set_source_pixbuf(cr, b->pixbuf, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		return;
	case BLK_CODE:
		setrgba(cr, colcodebg);
		cairo_rectangle(cr, margin, b->y, gtk_widget_get_allocated_width(canvas) - 2 * margin, b->h - blockgap);
		cairo_fill(cr);
		setrgba(cr, colcode);
		cairo_move_to(cr, margin + codepad, b->y + codepad);
		pango_cairo_show_layout(cr, b->layout);
		return;
	case BLK_TABLE: {
		int tablew = 0;
		for (int c = 0; c < b->ncols; c++)
			tablew += b->colw[c];

		for (int r = 0; r < b->nrows; r++) {
			int ry = b->y + b->rowy[r];
			if (r == 0) {
				setrgba(cr, coltablehead);
				cairo_rectangle(cr, margin, ry, tablew, b->rowh[r]);
				cairo_fill(cr);
			} else if (r % 2 == 0) {
				setrgba(cr, coltablealt);
				cairo_rectangle(cr, margin, ry, tablew, b->rowh[r]);
				cairo_fill(cr);
			}
			int cx = margin;
			for (int c = 0; c < b->ncols; c++) {
				Block *cell = b->cells[r * b->ncols + c];
				setrgba(cr, colfg);
				cairo_move_to(cr, cx + cellpad, ry + cellpad);
				pango_cairo_show_layout(cr, cell->layout);
				drawinlineimgs(cr, cell, cx + cellpad, ry + cellpad);
				cx += b->colw[c];
			}
		}

		setrgba(cr, coltableline);
		cairo_set_line_width(cr, 1);
		int cy = b->y;
		for (int r = 0; r <= b->nrows; r++) {
			cairo_move_to(cr, margin, cy + 0.5);
			cairo_line_to(cr, margin + tablew, cy + 0.5);
			cairo_stroke(cr);
			if (r < b->nrows)
				cy += b->rowh[r];
		}
		int cx = margin;
		for (int c = 0; c <= b->ncols; c++) {
			cairo_move_to(cr, cx + 0.5, b->y);
			cairo_line_to(cr, cx + 0.5, b->y + b->h - blockgap);
			cairo_stroke(cr);
			if (c < b->ncols)
				cx += b->colw[c];
		}
		return;
	}
	case BLK_QUOTE:
		setrgba(cr, colbar);
		cairo_rectangle(cr, margin, b->y, 3, b->h - blockgap);
		cairo_fill(cr);
		setrgba(cr, colquote);
		cairo_move_to(cr, x, b->y);
		pango_cairo_show_layout(cr, b->layout);
		drawinlineimgs(cr, b, x, b->y);
		return;
	case BLK_ITEM:
		setrgba(cr, colfg);
		cairo_move_to(cr, margin + 4, b->y);
		{
			PangoLayout *m = gtk_widget_create_pango_layout(canvas, NULL);
			char bullet[16];
			snprintf(bullet, sizeof bullet, b->ordered ? "%d." : "\xe2\x80\xa2", b->num);
			pango_layout_set_text(m, bullet, -1);
			pango_cairo_show_layout(cr, m);
			g_object_unref(m);
		}
		cairo_move_to(cr, x, b->y);
		pango_cairo_show_layout(cr, b->layout);
		drawinlineimgs(cr, b, x, b->y);
		return;
	case BLK_HEAD:
		setrgba(cr, colhead);
		cairo_move_to(cr, x, b->y);
		pango_cairo_show_layout(cr, b->layout);
		drawinlineimgs(cr, b, x, b->y);
		return;
	default:
		setrgba(cr, colfg);
		cairo_move_to(cr, x, b->y);
		pango_cairo_show_layout(cr, b->layout);
		drawinlineimgs(cr, b, x, b->y);
		return;
	}
}

static gboolean
ondraw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	double x0, y0, x1, y1;

	setrgba(cr, colbg);
	cairo_paint(cr);
	cairo_clip_extents(cr, &x0, &y0, &x1, &y1);

	for (Block *b = doc; b; b = b->next) {
		if (b->y + b->h < y0)
			continue;
		if (b->y > y1)
			break;
		drawblock(cr, b);
	}
	return TRUE;
}

static Block *
blockat(int y, int *localy)
{
	for (Block *b = doc; b; b = b->next)
		if (y >= b->y && y < b->y + b->h) {
			*localy = y - b->y;
			return b;
		}
	return NULL;
}

static const char *
linkinlayout(PangoLayout *layout, Link *links, int nlinks, int lx, int ly)
{
	int idx, trail;
	if (!layout || !pango_layout_xy_to_index(layout, lx * PANGO_SCALE, ly * PANGO_SCALE, &idx, &trail))
		return NULL;
	for (int i = 0; i < nlinks; i++)
		if (idx >= links[i].start && idx < links[i].end)
			return links[i].href;
	return NULL;
}

static const char *
hrefat(Block *b, int lx, int ly)
{
	if (b->type == BLK_TABLE) {
		if (lx < margin)
			return NULL;
		int cx = margin;
		for (int c = 0; c < b->ncols; c++) {
			if (lx < cx + b->colw[c]) {
				for (int r = 0; r < b->nrows; r++)
					if (ly >= b->rowy[r] && ly < b->rowy[r] + b->rowh[r]) {
						Block *cell = b->cells[r * b->ncols + c];
						return linkinlayout(cell->layout, cell->links, cell->nlinks,
							lx - cx - cellpad, ly - b->rowy[r] - cellpad);
					}
				return NULL;
			}
			cx += b->colw[c];
		}
		return NULL;
	}
	if (b->type != BLK_PARA && b->type != BLK_HEAD && b->type != BLK_ITEM && b->type != BLK_QUOTE)
		return NULL;
	return linkinlayout(b->layout, b->links, b->nlinks, lx - blockindent(b), ly);
}

static gboolean
onclick(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	int ly;
	Block *b = blockat((int)ev->y, &ly);
	if (b) {
		const char *href = hrefat(b, (int)ev->x, ly);
		if (href) {
			char *resolved = resolvepath(href);
			char *xopen[] = { "xdg-open", resolved ? resolved : (char *)href, NULL };
			spawn(xopen);
			g_free(resolved);
		}
	}
	return TRUE;
}

static gboolean
onmotion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
	int ly;
	Block *b = blockat((int)ev->y, &ly);
	const char *href = b ? hrefat(b, (int)ev->x, ly) : NULL;
	GdkCursor *cur = gdk_cursor_new_for_display(gdk_display_get_default(),
		href ? GDK_HAND2 : GDK_LEFT_PTR);
	gdk_window_set_cursor(gtk_widget_get_window(w), cur);
	g_object_unref(cur);
	return TRUE;
}

static void
invalidatecache(void)
{
	for (Block *b = doc; b; b = b->next) {
		if (b->layout) {
			g_object_unref(b->layout);
			b->layout = NULL;
		}
		if (b->pixbuf) {
			g_object_unref(b->pixbuf);
			b->pixbuf = NULL;
		}
		if (b->type == BLK_TABLE)
			for (int i = 0; i < b->ncols * b->nrows; i++)
				if (b->cells[i]->layout) {
					g_object_unref(b->cells[i]->layout);
					b->cells[i]->layout = NULL;
				}
	}
}

static gboolean
onresize(GtkWidget *w, GdkRectangle *alloc, gpointer data)
{
	static int lastw = -1;
	if (alloc->width != lastw) {
		lastw = alloc->width;
		invalidatecache();
		reflow();
	}
	return FALSE;
}

/* --- loading / reload --------------------------------------------------- */

static void
settitle(void)
{
	char *base = g_path_get_basename(filepath);
	char title[512];
	snprintf(title, sizeof title, "smd: %s%s", base,
		zoomfactor != 1.0 ? " *" : "");
	gtk_window_set_title(GTK_WINDOW(win), title);
	g_free(base);
}

static void
loadfile(void)
{
	gchar *content;
	gsize len;

	freedoc();
	if (g_file_get_contents(filepath, &content, &len, NULL)) {
		if (ishtml)
			parsehtml(content);
		else
			parsemd(content);
		g_free(content);
	} else {
		newblock(BLK_PARA)->raw = g_strdup_printf("couldn't read %s", filepath);
	}
	reflow();
	settitle();
	gtk_widget_queue_draw(canvas);
}

static gboolean
doreload(gpointer data)
{
	reloadsrc = 0;
	loadfile();
	return G_SOURCE_REMOVE;
}

static gboolean
oninotify(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	char buf[4096];
	ssize_t n = read(inotifyfd, buf, sizeof buf);
	char *fname = g_path_get_basename(filepath);
	int hit = 0;

	for (ssize_t off = 0; off < n; ) {
		struct inotify_event *ev = (struct inotify_event *)(buf + off);
		if (ev->len && !strcmp(ev->name, fname))
			hit = 1;
		off += sizeof *ev + ev->len;
	}
	g_free(fname);

	if (hit) {
		if (reloadsrc)
			g_source_remove(reloadsrc);
		reloadsrc = g_timeout_add(reloadwait, doreload, NULL);
	}
	return G_SOURCE_CONTINUE;
}

static void
watchfile(void)
{
	inotifyfd = inotify_init1(IN_NONBLOCK);
	if (inotifyfd < 0)
		return;
	/* watch the directory, not the file: editors that save-by-rename
	 * would otherwise silently drop our watch on the old inode */
	watchwd = inotify_add_watch(inotifyfd, basedir,
		IN_MODIFY | IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE);
	GIOChannel *chan = g_io_channel_unix_new(inotifyfd);
	g_io_add_watch(chan, G_IO_IN, oninotify, NULL);
	g_io_channel_unref(chan);
}

/* --- key bindings ------------------------------------------------------- */

static void
quit(const Arg *arg)
{
	gtk_main_quit();
}

static void
reload(const Arg *arg)
{
	invalidatecache();
	loadfile();
}

static void
scroll(const Arg *arg)
{
	double v = gtk_adjustment_get_value(vadj) + arg->i * scrollstep;
	gtk_adjustment_set_value(vadj, CLAMP(v, 0, gtk_adjustment_get_upper(vadj)));
}

static void
page(const Arg *arg)
{
	double v = gtk_adjustment_get_value(vadj) + arg->i * pagestep;
	gtk_adjustment_set_value(vadj, CLAMP(v, 0, gtk_adjustment_get_upper(vadj)));
}

static void
totop(const Arg *arg)
{
	gtk_adjustment_set_value(vadj, 0);
}

static void
tobottom(const Arg *arg)
{
	gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj));
}

static void
zoom(const Arg *arg)
{
	zoomfactor = CLAMP(zoomfactor + arg->f * zoomstep, zoommin, zoommax);
	invalidatecache();
	reflow();
	settitle();
	gtk_widget_queue_draw(canvas);
}

static void
zoomreset(const Arg *arg)
{
	zoomfactor = 1.0;
	invalidatecache();
	reflow();
	settitle();
	gtk_widget_queue_draw(canvas);
}

static gboolean
onkey(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	for (size_t i = 0; i < LENGTH(keys); i++)
		if (ev->keyval == keys[i].key &&
		    (ev->state & gtk_accelerator_get_default_mod_mask()) == keys[i].mod) {
			keys[i].func(&keys[i].arg);
			return TRUE;
		}
	return FALSE;
}

/* --- main ---------------------------------------------------------------- */

static void
usage(void)
{
	die("usage: %s [-v] file.md", argv0);
}

int
main(int argc, char *argv[])
{
	gtk_init(&argc, &argv);

	ARGBEGIN {
	case 'v':
		die("smd-"VERSION);
	default:
		usage();
	} ARGEND

	if (!argc)
		usage();

	filepath = g_canonicalize_filename(argv[0], NULL);
	basedir = g_path_get_dirname(filepath);
	{
		const char *dot = strrchr(filepath, '.');
		ishtml = dot && (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm"));
	}
	cachedir = g_build_filename(g_get_home_dir(), ".cache", "smd", NULL);
	g_mkdir_with_parents(cachedir, 0755);

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(win), 860, 720);
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(win, "key-press-event", G_CALLBACK(onkey), NULL);

	scroller = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_add(GTK_CONTAINER(win), scroller);
	vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));

	canvas = gtk_drawing_area_new();
	gtk_widget_add_events(canvas, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
	gtk_container_add(GTK_CONTAINER(scroller), canvas);
	g_signal_connect(canvas, "draw", G_CALLBACK(ondraw), NULL);
	g_signal_connect(canvas, "size-allocate", G_CALLBACK(onresize), NULL);
	g_signal_connect(canvas, "button-press-event", G_CALLBACK(onclick), NULL);
	g_signal_connect(canvas, "motion-notify-event", G_CALLBACK(onmotion), NULL);

	loadfile();
	watchfile();

	gtk_widget_show_all(win);
	gtk_main();

	freedoc();
	return 0;
}
