/* smd - config.h, edit + rebuild to customize (cp config.def.h config.h) */

/* fonts, pango format "Family [style] size" */
static const char *fontbody = "Sans 11";
static const char *fontmono = "Monospace 10";
static const char *fonthead[6] = {
	"Sans Bold 22", "Sans Bold 19", "Sans Bold 16",
	"Sans Bold 14", "Sans Bold 12", "Sans Bold 11",
};

/* colors, #rrggbb. default is a paper-ish light theme, easy on the eyes
 * for reading long docs. flip bg/fg for a dark one if that's your thing */
static const char *colbg      = "#fbf1c7";
static const char *colfg      = "#3c3836";
static const char *colhead    = "#9d0006";
static const char *collink    = "#076678";
static const char *colcode    = "#3c3836";
static const char *colcodebg  = "#ebdbb2";
static const char *colquote   = "#665c54";
static const char *colbar     = "#928374"; /* blockquote bar + hr */
static const char *coltablehead = "#d5c4a1";
static const char *coltablealt  = "#f2e5bc";
static const char *coltableline = "#bdae93";

static const int cellpad = 8;

static const int margin      = 28;  /* left/right/top page margin, px */
static const int blockgap    = 14;  /* vertical gap between blocks, px */
static const int quoteindent = 16;  /* blockquote left indent, px */
static const int listindent  = 22;  /* list item indent, px */
static const int codepad     = 8;   /* padding inside code blocks, px */
static const int hrthick     = 2;   /* hr line thickness, px */

static const int mathdpi     = 300; /* latex rasterization dpi */
static const double zoomstep = 0.1;
static const double zoommin  = 0.5;
static const double zoommax  = 3.0;

static const int scrollstep  = 46;  /* px per j/k */
static const int pagestep    = 420; /* px per space/pgup/pgdn */

/* poll interval for the inotify watch, ms. only matters for debouncing
 * editors that write files in bursts (vim swap dance etc) */
static const int reloadwait  = 120;

/* Arg/Key are typedef'd in smd.c, quit/reload/scroll/etc are declared
 * there too - config.h just wires them together */
/* modifier      keyval             function       arg */
static Key keys[] = {
	{ 0,          GDK_KEY_q,         quit,          {0} },
	{ 0,          GDK_KEY_r,         reload,        {0} },
	{ 0,          GDK_KEY_j,         scroll,        { .i = +1 } },
	{ 0,          GDK_KEY_k,         scroll,        { .i = -1 } },
	{ 0,          GDK_KEY_Down,      scroll,        { .i = +1 } },
	{ 0,          GDK_KEY_Up,        scroll,        { .i = -1 } },
	{ 0,          GDK_KEY_space,     page,          { .i = +1 } },
	{ GDK_SHIFT_MASK, GDK_KEY_space, page,          { .i = -1 } },
	{ 0,          GDK_KEY_Page_Down, page,          { .i = +1 } },
	{ 0,          GDK_KEY_Page_Up,   page,          { .i = -1 } },
	{ 0,          GDK_KEY_g,         totop,         {0} },
	{ GDK_SHIFT_MASK, GDK_KEY_G,     tobottom,      {0} },
	{ 0,          GDK_KEY_plus,      zoom,          { .f = +1 } },
	{ 0,          GDK_KEY_equal,     zoom,          { .f = +1 } },
	{ 0,          GDK_KEY_minus,     zoom,          { .f = -1 } },
	{ 0,          GDK_KEY_0,         zoomreset,     {0} },
};
