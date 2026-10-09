/*
 * sfetch - neofetch-style ASCII radar chart over your todo.txt stats.
 *
 * Task management is NOT done by this tool - use tuxedo
 * (https://github.com/webstonehq/tuxedo) or any other todo.txt-cli
 * compatible tool for add/do/list. sfetch only reads the resulting
 * todo.txt / done.txt files and renders a 5-attribute radar chart.
 *
 * Convention: mark a task with `!<code>` (a single character) to give
 * one point to the matching attribute. Only completed ("x "-prefixed)
 * lines are scanned, and a marker must be its own whitespace-separated
 * token (e.g. "!k", not "!knowledge" or "studied!k").
 *
 *   tuxedo add "finish OS assignment +school !k"
 *   tuxedo do 3
 *   sfetch
 *
 * Multiple markers on one line are all counted, so a single task can
 * feed several attributes at once: "helped a friend study !k !n".
 *
 * Points accumulate uncapped, but the chart and the per-attribute stat
 * bars both read off a derived Level (1-5, Persona-confidant-style)
 * rather than raw points. Level 1 is the floor - zero points is still
 * Level 1, never Level 0. Thresholds: 0 / 10 / 25 / 50 / 100 points.
 *
 * File resolution mirrors tuxedo's own:
 *   todo file: $TODO_FILE ->$TODO_DIR/todo.txt -> ./todo.txt
 *   done file: $DONE_FILE -> <todo dir>/done.txt
 *
 * Storage owned by sfetch itself:
 *   ~/.config/sfetch/attributes.conf   - "Name:code" per line, 5 lines
 *
 * Build: make
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#define NUM_ATTRS 5
#define ATTR_LEN 24
#define PATH_LEN 512
#define LINE_LEN 1024
#define MAX_POINTS 100 /* hard ceiling on points per attribute */

typedef struct {
  char name[ATTR_LEN];
  char code; /* lowercase single char, matched case-insensitively */
  int total;
  int marker_count;
} Attr;

static Attr attrs[NUM_ATTRS];

static char config_dir[PATH_LEN];
static char attrs_path[PATH_LEN];

typedef struct {
  const char *name;
  char code;
} DefaultAttr;
static const DefaultAttr default_attrs[NUM_ATTRS] = {
    {"Knowledge", 'k'}, {"Vitality", 'v'},    {"Diligence", 'd'},
    {"Charm", 'c'},     {"Proficiency", 'p'},
};

static int completed_total = 0;
static int completed_unmatched = 0; /* done tasks with no recognized !code */

/* ---------- path / fs helpers ---------- */

static void ensure_dir(const char *path) {
  char tmp[PATH_LEN];
  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t len = strlen(tmp);
  if (len && tmp[len - 1] == '/')
    tmp[len - 1] = '\0';

  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "sfetch: mkdir %s: %s\n", tmp, strerror(errno));
        exit(1);
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "sfetch: mkdir %s: %s\n", tmp, strerror(errno));
    exit(1);
  }
}

static void init_sfetch_paths(void) {
  const char *home = getenv("HOME");
  if (!home) {
    fprintf(stderr, "sfetch: $HOME not set\n");
    exit(1);
  }
  snprintf(config_dir, sizeof(config_dir), "%s/.config/sfetch", home);
  snprintf(attrs_path, sizeof(attrs_path), "%s/attributes.conf", config_dir);
  ensure_dir(config_dir);
}

static int file_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0;
}

static void dir_of(const char *path, char *out, size_t out_sz) {
  const char *slash = strrchr(path, '/');
  if (!slash) {
    snprintf(out, out_sz, ".");
    return;
  }
  size_t len = (size_t)(slash - path);
  if (len == 0)
    len = 1;
  if (len >= out_sz)
    len = out_sz - 1;
  memcpy(out, path, len);
  out[len] = '\0';
}

static int resolve_todo_path(char *out, size_t out_sz) {
  const char *todo_file = getenv("TODO_FILE");
  if (todo_file && todo_file[0]) {
    snprintf(out, out_sz, "%s", todo_file);
    return file_exists(out);
  }
  const char *todo_dir = getenv("TODO_DIR");
  if (todo_dir && todo_dir[0]) {
    snprintf(out, out_sz, "%s/todo.txt", todo_dir);
    return file_exists(out);
  }
  snprintf(out, out_sz, "./todo.txt");
  return file_exists(out);
}

static void resolve_done_path(const char *todo_path, char *out, size_t out_sz) {
  const char *done_file = getenv("DONE_FILE");
  if (done_file && done_file[0]) {
    snprintf(out, out_sz, "%s", done_file);
    return;
  }
  char dir[PATH_LEN];
  dir_of(todo_path, dir, sizeof(dir));
  snprintf(out, out_sz, "%s/done.txt", dir);
}

/* ---------- attributes.conf: "Name:code" per line ---------- */

static void set_defaults(void) {
  for (int i = 0; i < NUM_ATTRS; i++) {
    strncpy(attrs[i].name, default_attrs[i].name, ATTR_LEN - 1);
    attrs[i].name[ATTR_LEN - 1] = '\0';
    attrs[i].code = default_attrs[i].code;
    attrs[i].total = 0;
    attrs[i].marker_count = 0;
  }
}

static void load_attrs(void) {
  set_defaults();

  FILE *f = fopen(attrs_path, "r");
  if (!f)
    return;

  char line[128];
  int i = 0;
  while (i < NUM_ATTRS && fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0')
      continue;
    char *colon = strchr(line, ':');
    if (!colon || colon[1] == '\0')
      continue; /* malformed, skip line */
    *colon = '\0';
    strncpy(attrs[i].name, line, ATTR_LEN - 1);
    attrs[i].name[ATTR_LEN - 1] = '\0';
    attrs[i].code = (char)tolower((unsigned char)colon[1]);
    i++;
  }
  fclose(f);
}

static void save_attrs(void) {
  FILE *f = fopen(attrs_path, "w");
  if (!f) {
    perror("sfetch: save_attrs");
    return;
  }
  for (int i = 0; i < NUM_ATTRS; i++)
    fprintf(f, "%s:%c\n", attrs[i].name, attrs[i].code);
  fclose(f);
}

static int find_attr_by_code(char c) {
  char lc = (char)tolower((unsigned char)c);
  for (int i = 0; i < NUM_ATTRS; i++)
    if (attrs[i].code == lc)
      return i;
  return -1;
}

static int codes_have_duplicates(void) {
  for (int i = 0; i < NUM_ATTRS; i++)
    for (int j = i + 1; j < NUM_ATTRS; j++)
      if (attrs[i].code == attrs[j].code)
        return 1;
  return 0;
}

/* ---------- todo.txt / done.txt parsing ---------- */

/* A marker token is exactly "!" followed by one code character - nothing
 * else in the token. Reject "!knowledge", "!k2", "word!k", etc. */
static void process_completed_line(const char *line) {
  char buf[LINE_LEN];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  int has_any = 0;

  char *saveptr;
  char *tok = strtok_r(buf, " \t", &saveptr);
  while (tok) {
    if (tok[0] == '!' && tok[1] != '\0' && tok[2] == '\0') {
      int idx = find_attr_by_code(tok[1]);
      if (idx >= 0) {
        if (attrs[idx].total < MAX_POINTS)
          attrs[idx].total++;
        attrs[idx].marker_count++;
        has_any = 1;
      }
    }
    tok = strtok_r(NULL, " \t", &saveptr);
  }

  completed_total++;
  if (!has_any)
    completed_unmatched++;
}

static void scan_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;

  char line[LINE_LEN];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0')
      continue;
    if (line[0] == 'x' && line[1] == ' ') {
      process_completed_line(line + 2);
    }
  }
  fclose(f);
}

/* ---------- commands ---------- */

static void print_usage(void) {
  printf(
      "sfetch - ASCII radar chart fetch display over todo.txt stats\n\n"
      "Task management lives in tuxedo (or any todo.txt-cli tool), not here:\n"
      "  tuxedo add \"task text +project !k\"\n"
      "  tuxedo do <n>\n\n"
      "Each !<code> marker on a completed task is worth 1 point.\n\n"
      "usage:\n"
      "  sfetch                            chart + system info (default)\n"
      "  sfetch -t                         chart + task/attribute stats\n"
      "  sfetch fetch                      same as sfetch\n"
      "  sfetch fetch -t                   same as sfetch -t\n"
      "  sfetch attrs                      show current attributes + codes\n"
      "  sfetch attrs N1:c1 ... N5:c5      rename all 5 (name:code pairs)\n");
}

static void cmd_attrs(int argc, char **argv) {
  if (argc >= 3) {
    if (argc < 2 + NUM_ATTRS) {
      fprintf(stderr, "sfetch: provide all %d attributes as Name:code\n",
              NUM_ATTRS);
      exit(1);
    }
    Attr proposed[NUM_ATTRS];
    for (int i = 0; i < NUM_ATTRS; i++) {
      char *arg = argv[2 + i];
      char *colon = strchr(arg, ':');
      if (!colon || colon[1] == '\0' || colon[2] != '\0') {
        fprintf(stderr,
                "sfetch: '%s' must look like Name:c (single-char code)\n", arg);
        exit(1);
      }
      *colon = '\0';
      strncpy(proposed[i].name, arg, ATTR_LEN - 1);
      proposed[i].name[ATTR_LEN - 1] = '\0';
      proposed[i].code = (char)tolower((unsigned char)colon[1]);
    }
    for (int i = 0; i < NUM_ATTRS; i++) {
      strncpy(attrs[i].name, proposed[i].name, ATTR_LEN - 1);
      attrs[i].code = proposed[i].code;
    }
    if (codes_have_duplicates()) {
      fprintf(stderr, "sfetch: codes must be unique across all 5 attributes\n");
      exit(1);
    }
    save_attrs();
    printf("attributes updated. note: this does not retag tasks already\n"
           "written with the old codes - matching is against current codes "
           "only.\n\n");
  }
  printf("current attributes:\n");
  for (int i = 0; i < NUM_ATTRS; i++)
    printf("  %d. %-12s !%c\n", i + 1, attrs[i].name, attrs[i].code);
}

/* ---------- radar chart ---------- */

#define GRID_W 61
#define GRID_H 27

#define COLOR_RESET "\x1b[0m"

/* 5 discrete brightness tiers (not a smooth ramp) - tier 1 is the dimmest
   tier actually used, deliberately stopping short of near-black (232) so
   the far edge of the fill stays visible against a black terminal bg */
#define NUM_GRAY_LEVELS 6
static const int gray_levels[NUM_GRAY_LEVELS] = {235, 239, 243, 247, 250, 253};

/* Level system: 1-6, one layer/ring per level. Level 1 is the permanent
   floor (0 points still reads as Level 1) but sits at the FIRST ring out
   from center, not dead center - see the ratio calc in render_radar.
   Thresholds grow exponentially (roughly doubling gaps) and top out at
   93, just under the 100-point-per-attribute cap. */
#define NUM_LEVELS 6
static const int level_thresholds[NUM_LEVELS] = {0, 3, 9, 21, 45, 93};

static int compute_level(int total) {
  int lvl = 1;
  for (int i = 0; i < NUM_LEVELS; i++)
    if (total >= level_thresholds[i])
      lvl = i + 1;
  return lvl;
}

/* cell contents, drawn as real glyphs rather than plain ASCII */
enum { CELL_EMPTY = 0, CELL_GUIDE, CELL_EDGE, CELL_VERTEX, CELL_CENTER };
static const char *glyph[] = {" ", "\u00b7", "\u25cf", "\u25c6", "\u2726"};
/*                            empty  guide ·   edge ●    vertex ◆  center ✦ */

/* standard ray-casting point-in-polygon test */
static int point_in_poly(double px, double py, const int *vx, const int *vy,
                         int n) {
  int inside = 0;
  int j = n - 1;
  for (int i = 0; i < n; i++) {
    if (((vy[i] > py) != (vy[j] > py)) &&
        (px < (double)(vx[j] - vx[i]) * (py - vy[i]) / (double)(vy[j] - vy[i]) +
                  vx[i]))
      inside = !inside;
    j = i;
  }
  return inside;
}

static void plot(int g[GRID_H][GRID_W], int x, int y, int type) {
  if (x >= 0 && x < GRID_W && y >= 0 && y < GRID_H)
    if (g[y][x] == CELL_EMPTY || type == CELL_EDGE || type == CELL_VERTEX ||
        type == CELL_CENTER)
      g[y][x] = type;
}

static void draw_line(int g[GRID_H][GRID_W], int x0, int y0, int x1, int y1,
                      int type) {
  int dx = x1 - x0, dy = y1 - y0;
  int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
  if (steps == 0) {
    plot(g, x0, y0, type);
    return;
  }
  for (int i = 0; i <= steps; i++) {
    double t = (double)i / steps;
    int x = x0 + (int)lround(dx * t);
    int y = y0 + (int)lround(dy * t);
    plot(g, x, y, type);
  }
}

#define ROW_BUF_LEN 2048

static void render_radar(char rows[GRID_H][ROW_BUF_LEN]) {
  int grid[GRID_H][GRID_W];
  for (int y = 0; y < GRID_H; y++)
    for (int x = 0; x < GRID_W; x++)
      grid[y][x] = CELL_EMPTY;

  int cx = GRID_W / 2;
  int cy = GRID_H / 2;
  double max_r_y = (GRID_H / 2) - 2;
  double max_r_x = max_r_y * 2.0;

  double angle_deg[NUM_ATTRS];
  int axis_x[NUM_ATTRS], axis_y[NUM_ATTRS];
  int data_x[NUM_ATTRS], data_y[NUM_ATTRS];

  for (int i = 0; i < NUM_ATTRS; i++) {
    angle_deg[i] = -90.0 + i * (360.0 / NUM_ATTRS);
    double rad = angle_deg[i] * M_PI / 180.0;

    axis_x[i] = cx + (int)lround(max_r_x * cos(rad));
    axis_y[i] = cy + (int)lround(max_r_y * sin(rad));

    /* Level-driven, not point-driven: Level 1 sits at the FIRST ring
       out from center (ratio 1/6), not dead center - Level 6 reaches
       the outer ring. */
    int lvl = compute_level(attrs[i].total);
    double ratio = (double)lvl / (double)NUM_LEVELS;

    data_x[i] = cx + (int)lround(max_r_x * ratio * cos(rad));
    data_y[i] = cy + (int)lround(max_r_y * ratio * sin(rad));
  }

  /* axis spokes, center to outer ring */
  for (int i = 0; i < NUM_ATTRS; i++)
    draw_line(grid, cx, cy, axis_x[i], axis_y[i], CELL_GUIDE);

  /* 6 concentric rings, one per layer - a proper spider-chart grid
     instead of just a single outer reference pentagon */
  for (int r = 1; r <= NUM_LEVELS; r++) {
    double rr = (double)r / NUM_LEVELS;
    int ring_x[NUM_ATTRS], ring_y[NUM_ATTRS];
    for (int i = 0; i < NUM_ATTRS; i++) {
      double rad = angle_deg[i] * M_PI / 180.0;
      ring_x[i] = cx + (int)lround(max_r_x * rr * cos(rad));
      ring_y[i] = cy + (int)lround(max_r_y * rr * sin(rad));
    }
    for (int i = 0; i < NUM_ATTRS; i++) {
      int j = (i + 1) % NUM_ATTRS;
      draw_line(grid, ring_x[i], ring_y[i], ring_x[j], ring_y[j], CELL_GUIDE);
    }
  }

  for (int i = 0; i < NUM_ATTRS; i++) {
    int j = (i + 1) % NUM_ATTRS;
    draw_line(grid, data_x[i], data_y[i], data_x[j], data_y[j], CELL_EDGE);
  }
  for (int i = 0; i < NUM_ATTRS; i++)
    plot(grid, data_x[i], data_y[i], CELL_VERTEX);

  plot(grid, cx, cy, CELL_CENTER);

  int use_color = isatty(STDOUT_FILENO);

  char interior[GRID_H][GRID_W];
  for (int y = 0; y < GRID_H; y++)
    for (int x = 0; x < GRID_W; x++)
      interior[y][x] = use_color && point_in_poly((double)x, (double)y, data_x,
                                                  data_y, NUM_ATTRS);

  for (int y = 0; y < GRID_H; y++) {
    size_t pos = 0;
    rows[y][0] = '\0';
    for (int x = 0; x < GRID_W; x++) {
      int written;
      if (interior[y][x]) {
        /* radial brightness falloff from center, donut.c-style:
           undo the x-axis aspect stretch first so the gradient
           reads as circular rather than elliptical */
        double ddx = (x - cx) / 2.0;
        double ddy = (y - cy);
        double dist = sqrt(ddx * ddx + ddy * ddy);
        double ratio = max_r_y > 0 ? dist / max_r_y : 0.0;
        if (ratio > 1.0)
          ratio = 1.0;

        /* quantize into 5 discrete tiers: ratio 0 (center) -> tier 5
           (brightest), ratio ~1 (outer edge) -> tier 1 (dimmest) */
        int bucket = (int)(ratio * NUM_GRAY_LEVELS);
        if (bucket >= NUM_GRAY_LEVELS)
          bucket = NUM_GRAY_LEVELS - 1;
        int tier_from_center = (NUM_GRAY_LEVELS - 1) - bucket;
        written =
            snprintf(rows[y] + pos, ROW_BUF_LEN - pos, "\x1b[48;5;%dm%s\x1b[0m",
                     gray_levels[tier_from_center], glyph[grid[y][x]]);
      } else {
        written =
            snprintf(rows[y] + pos, ROW_BUF_LEN - pos, "%s", glyph[grid[y][x]]);
      }
      if (written > 0 && (size_t)written < ROW_BUF_LEN - pos)
        pos += (size_t)written;
    }
  }
}

/* ---------- system stats (neofetch-style) ---------- */

static void get_os_name(char *out, size_t out_sz) {
  FILE *f = fopen("/etc/os-release", "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
        char *start = strchr(line, '"');
        char *end = start ? strrchr(line, '"') : NULL;
        if (start && end && end > start) {
          size_t len = (size_t)(end - start - 1);
          if (len >= out_sz)
            len = out_sz - 1;
          memcpy(out, start + 1, len);
          out[len] = '\0';
          fclose(f);
          return;
        }
      }
    }
    fclose(f);
  }
  struct utsname u;
  snprintf(out, out_sz, "%s", uname(&u) == 0 ? u.sysname : "Unknown");
}

static void get_uptime(char *out, size_t out_sz) {
  FILE *f = fopen("/proc/uptime", "r");
  if (!f) {
    snprintf(out, out_sz, "unknown");
    return;
  }
  double secs = 0;
  if (fscanf(f, "%lf", &secs) != 1)
    secs = 0;
  fclose(f);
  long total = (long)secs;
  long days = total / 86400;
  long hours = (total % 86400) / 3600;
  long mins = (total % 3600) / 60;
  if (days > 0)
    snprintf(out, out_sz, "%ldd %ldh %ldm", days, hours, mins);
  else if (hours > 0)
    snprintf(out, out_sz, "%ldh %ldm", hours, mins);
  else
    snprintf(out, out_sz, "%ldm", mins);
}

#define RULE                                                                   \
  "\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501"   \
  "\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501"   \
  "\u2501\u2501\u2501\u2501\u2501"

#define MAX_INFO_LINES 24
#define INFO_LINE_LEN 256

static char info_lines[MAX_INFO_LINES][INFO_LINE_LEN];
static int info_line_count = 0;

static void add_info_line(const char *fmt, ...) {
  if (info_line_count >= MAX_INFO_LINES)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(info_lines[info_line_count], INFO_LINE_LEN, fmt, ap);
  va_end(ap);
  info_line_count++;
}

/* system info block (default mode) */
static void build_system_info(const char *todo_path) {
  info_line_count = 0;
  char *user = getenv("USER");

  struct utsname u;
  int have_uname = uname(&u) == 0;
  char os_name[128];
  get_os_name(os_name, sizeof(os_name));
  char uptime_str[64];
  get_uptime(uptime_str, sizeof(uptime_str));

  add_info_line("");
  add_info_line("");
  add_info_line("");
  add_info_line("");
  add_info_line("");
  add_info_line("");
  add_info_line("");
  add_info_line("");
  add_info_line("%s@sfetch", user ? user : "you");
  add_info_line("%s", RULE);
  add_info_line("source: %s", todo_path);
  add_info_line("OS:      %s", os_name);
  add_info_line("Kernel:  %s", have_uname ? u.release : "unknown");
  add_info_line("Uptime:  %s", uptime_str);
  add_info_line("Shell:   zsh"); /* hardcoded, not read from $SHELL */
  add_info_line("Host:    %s", have_uname ? u.nodename : "unknown");
  add_info_line("%s", RULE);
}

/* task/attribute info block (-t mode) */
static void build_task_info(const char *todo_path) {
  info_line_count = 0;
  char *user = getenv("USER");

  add_info_line("");
  add_info_line("");
  add_info_line("");
  add_info_line("");
  add_info_line("");
  add_info_line("");
  add_info_line("%s@sfetch", user ? user : "you");
  add_info_line("%s", RULE);
  add_info_line("source: %s", todo_path);
  add_info_line("");

  for (int i = 0; i < NUM_ATTRS; i++) {
    int lvl = compute_level(attrs[i].total);
    char bar[4 * NUM_LEVELS +
             1]; /* each glyph is 3 UTF-8 bytes; pad for safety */
    bar[0] = '\0';
    for (int j = 0; j < NUM_LEVELS; j++)
      strcat(bar, j < lvl ? "\u25a0" : "\u25a1"); /* filled/empty square */
    add_info_line("%-12s !%c  %s  Lv.%d  (%d pts)", attrs[i].name,
                  attrs[i].code, bar, lvl, attrs[i].total);
  }

  add_info_line("");
  if (completed_unmatched > 0)
    add_info_line("tasks: %d done, %d unlabeled", completed_total,
                  completed_unmatched);
  else
    add_info_line("tasks: %d done", completed_total);
  add_info_line("%s", RULE);
}

static void cmd_fetch(int show_tasks) {
  char todo_path[PATH_LEN];
  char done_path[PATH_LEN];

  int found = resolve_todo_path(todo_path, sizeof(todo_path));
  if (!found) {
    fprintf(stderr,
            "sfetch: no todo.txt found ($TODO_FILE / $TODO_DIR/todo.txt / "
            "./todo.txt)\n"
            "sfetch: create one with tuxedo first, e.g.:\n"
            "sfetch:   tuxedo add \"first task !%c\"\n",
            attrs[0].code);
    exit(1);
  }
  resolve_done_path(todo_path, done_path, sizeof(done_path));

  scan_file(todo_path);
  if (file_exists(done_path))
    scan_file(done_path);

  static char chart_rows[GRID_H][ROW_BUF_LEN];
  render_radar(chart_rows);

  if (show_tasks)
    build_task_info(todo_path);
  else
    build_system_info(todo_path);

  int total_rows = GRID_H > info_line_count ? GRID_H : info_line_count;
  for (int r = 0; r < total_rows; r++) {
    if (r < GRID_H)
      fputs(chart_rows[r], stdout);
    else
      for (int x = 0; x < GRID_W; x++)
        putchar(' ');
    fputs("  ", stdout);
    if (r < info_line_count)
      fputs(info_lines[r], stdout);
    putchar('\n');
  }
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
  init_sfetch_paths();
  load_attrs();

  int show_tasks = 0;
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "-t") == 0)
      show_tasks = 1;

  int is_fetch_call =
      argc < 2 || strcmp(argv[1], "fetch") == 0 || strcmp(argv[1], "-t") == 0;

  if (is_fetch_call) {
    cmd_fetch(show_tasks);
  } else if (strcmp(argv[1], "attrs") == 0) {
    cmd_attrs(argc, argv);
  } else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
    print_usage();
  } else {
    fprintf(stderr, "sfetch: unknown command '%s'\n\n", argv[1]);
    print_usage();
    exit(1);
  }

  return 0;
}
