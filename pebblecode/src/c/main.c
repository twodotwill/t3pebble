#include <pebble.h>

#define KEY_CMD 0
#define KEY_INDEX 1
#define KEY_TOTAL 2
#define KEY_SESSION_ID 3
#define KEY_TITLE 4
#define KEY_DIRECTORY 5
#define KEY_AGENT 6
#define KEY_STATUS 7
#define KEY_SUMMARY 8
#define KEY_CONTEXT 9
#define KEY_PROMPT 10
#define KEY_ERROR 11
#define KEY_REQUEST_ID 12
#define KEY_REQUEST_KIND 13
#define KEY_PROJECT_ID 14
#define KEY_CONTEXT_PAGE 15
#define KEY_SERVER 16
#define KEY_STATE 17
#define KEY_HOST_ID 18
#define KEY_DETAIL 19
#define KEY_C_NEEDS 20
#define KEY_C_RUN 21
#define KEY_C_IDLE 22
#define KEY_C_SETTLED 23

#define CMD_REFRESH 1
#define CMD_SESSION_ITEM 2
#define CMD_SESSION_END 3
#define CMD_DETAIL 4
#define CMD_PROMPT 5
#define CMD_ERROR 6
#define CMD_CONTEXT 7
#define CMD_STATUS 8
#define CMD_PROJECT_ITEM 9
#define CMD_PROJECT_END 10
#define CMD_NEW_THREAD 11
#define CMD_HOST_ITEM 12
#define CMD_HOST_END 13
#define CMD_SELECT_HOST 14

#define MAX_HOSTS 6
#define MAX_SESSIONS 20
#define MAX_PROJECTS 20
#define MAX_CONTEXT_TEXT 640
#define REFRESH_INTERVAL_MS 300000
#define BUILD_LABEL "v0.5.0"
#define DETAIL_CONTENT_HEIGHT 900
#define DETAIL_CONTENT_MIN_HEIGHT 120

/* The reference watch is a chassis around an inset LCD: a legend band, a
   crimson rail pointing at the buttons, the glass, then the same in reverse.
   Both bands are kept, but every character on them is live data rather than a
   model name. */
#define LEGEND_HEIGHT 14
#define RAIL_HEIGHT 2
#define TOP_CHROME (LEGEND_HEIGHT + RAIL_HEIGHT)
#define BOTTOM_CHROME (LEGEND_HEIGHT + RAIL_HEIGHT)
#ifdef PBL_ROUND
#define PANEL_INSET 16
#else
#define PANEL_INSET 3
#endif
#define ROW_HEIGHT 40
#define SECTION_HEADER_HEIGHT 15
#define CARD_PAD 8

#define STREAM_TICK_MS 110
#define GHOST_ROWS 4

/* Seven-segment bit order: a b c d e f g. */
#define SEG_A 0x01
#define SEG_B 0x02
#define SEG_C 0x04
#define SEG_D 0x08
#define SEG_E 0x10
#define SEG_F 0x20
#define SEG_G 0x40
#define SEG_ALL 0x7F

typedef struct {
  char id[40];
  char title[40];
  char state[10];
  int needs;
  int run;
  int idle;
  int settled;
} HostItem;

typedef struct {
  char id[80];
  char title[56];
  char detail[36];
  char state[10];
  char request_id[72];
  char request_kind[12];
  char summary[180];
} SessionItem;

typedef struct {
  char id[80];
  char title[52];
  char directory[40];
} ProjectItem;

typedef enum {
  DictationTargetSession,
  DictationTargetProject
} DictationTarget;

static Window *s_host_window;
static Window *s_thread_window;
static Window *s_detail_window;
static MenuLayer *s_thread_menu;
static Layer *s_host_layer;
static Layer *s_thread_chrome;
static Layer *s_detail_header_layer;
static Layer *s_detail_layer;
static ScrollLayer *s_scroll_layer;
static AppTimer *s_refresh_timer;
static AppTimer *s_stream_timer;
static DictationSession *s_dictation;
static DictationTarget s_dictation_target = DictationTargetSession;

static HostItem s_hosts[MAX_HOSTS];
static SessionItem s_sessions[MAX_SESSIONS];
static ProjectItem s_projects[MAX_PROJECTS];

static int s_host_count;
static int s_session_count;
static int s_project_count;
static int s_host_cursor;
static int s_selected_host_index = -1;
static int s_selected_index = -1;
static int s_selected_project_index = -1;
static int s_context_page;
static int s_context_page_count = 1;
static int s_pending_context_page;
static int s_context_request_counter;
static int s_stream_phase;

static bool s_loading_hosts;
static bool s_loading_threads;
static bool s_hosts_synced;
static bool s_threads_synced;
static bool s_showing_context;
static bool s_context_loading;
static bool s_context_page_nav;
static bool s_sending_message;
static bool s_link_down;
static time_t s_last_sync;

static char s_status_text[40];
static char s_full_context[MAX_CONTEXT_TEXT];
static char s_pending_context_session_id[80];
static char s_pending_context_request_id[16];
static char s_draw_body[MAX_CONTEXT_TEXT];

static void request_refresh(void);
static void request_detail(int index, bool full_context);
static void request_context_page(int page);
static void send_prompt_text(const char *text);
static void send_new_thread_text(const char *text);
static void start_dictation(void);
static void schedule_refresh(uint32_t delay_ms);
static void update_stream_timer(void);
static void detail_exit_context(bool animated);
static void detail_content_update_proc(Layer *layer, GContext *ctx);
static void update_detail_text(void);
static void detail_reset_scroll(bool animated);
static void open_selected_host(void);

/* ---------------------------------------------------------------- palette */

/* A negative-LCD Casio: near-black chassis, pale glass, dark ink, cyan
   legends, crimson rails. Monochrome watches collapse to black on white,
   which the shape language survives. */
static GColor lcd_glass(void) {
#ifdef PBL_COLOR
  return GColorLightGray;
#else
  return GColorWhite;
#endif
}

static GColor lcd_ink(void) {
  return GColorBlack;
}

static GColor lcd_dim(void) {
#ifdef PBL_COLOR
  return GColorDarkGray;
#else
  return GColorBlack;
#endif
}

static GColor chassis(void) {
  return GColorBlack;
}

static GColor legend(void) {
#ifdef PBL_COLOR
  return GColorPictonBlue;
#else
  return GColorWhite;
#endif
}

static GColor rule_color(void) {
#ifdef PBL_COLOR
  return GColorDarkCandyAppleRed;
#else
  return GColorWhite;
#endif
}

static GColor alert_color(void) {
#ifdef PBL_COLOR
  return GColorDarkCandyAppleRed;
#else
  return GColorBlack;
#endif
}

static GColor active_color(void) {
#ifdef PBL_COLOR
  return GColorCobaltBlue;
#else
  return GColorBlack;
#endif
}

static GFont font_row_title(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
}

static GFont font_row_detail(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_14);
}

static GFont font_legend(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
}

static GFont font_body(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_18);
}

/* -------------------------------------------------------------- utilities */

static bool state_is(const char *state, const char *match) {
  return strcmp(state, match) == 0;
}

static bool state_is_live(const char *state) {
  return state_is(state, "run");
}

static void copy_tuple(char *dest, size_t size, DictionaryIterator *iter, uint32_t key) {
  Tuple *tuple = dict_find(iter, key);
  if (!tuple || size == 0) {
    return;
  }
  strncpy(dest, tuple->value->cstring, size - 1);
  dest[size - 1] = '\0';
}

static int int_tuple(DictionaryIterator *iter, uint32_t key, int fallback) {
  Tuple *tuple = dict_find(iter, key);
  return tuple ? (int)tuple->value->int32 : fallback;
}

static int clamp_int(int value, int min, int max) {
  if (value < min) {
    return min;
  }
  if (value > max) {
    return max;
  }
  return value;
}

static bool host_window_visible(void) {
  return s_host_window && window_stack_get_top_window() == s_host_window;
}

static bool thread_window_visible(void) {
  return s_thread_window && window_stack_get_top_window() == s_thread_window;
}

static bool detail_window_visible(void) {
  return s_detail_window && window_stack_get_top_window() == s_detail_window;
}

static void mark_all_dirty(void) {
  if (s_host_layer) {
    layer_mark_dirty(s_host_layer);
  }
  if (s_thread_menu) {
    menu_layer_reload_data(s_thread_menu);
  }
  if (s_thread_chrome) {
    layer_mark_dirty(s_thread_chrome);
  }
  if (s_detail_header_layer) {
    layer_mark_dirty(s_detail_header_layer);
  }
}

static void set_status(const char *text) {
  strncpy(s_status_text, text, sizeof(s_status_text) - 1);
  s_status_text[sizeof(s_status_text) - 1] = '\0';
  mark_all_dirty();
}

static bool any_live_row(void) {
  for (int i = 0; i < s_session_count; i++) {
    if (state_is_live(s_sessions[i].state)) {
      return true;
    }
  }
  for (int i = 0; i < s_host_count; i++) {
    if (s_hosts[i].run > 0) {
      return true;
    }
  }
  return false;
}

static bool animation_active(void) {
  return s_loading_hosts || s_loading_threads || s_context_loading || s_sending_message ||
         !s_hosts_synced || any_live_row();
}

static void stream_timer_callback(void *context) {
  s_stream_timer = NULL;
  s_stream_phase = (s_stream_phase + 1) % 240;

  if (host_window_visible() && s_host_layer) {
    layer_mark_dirty(s_host_layer);
  }
  if (thread_window_visible()) {
    if (s_thread_chrome) {
      layer_mark_dirty(s_thread_chrome);
    }
    if (s_thread_menu) {
      layer_mark_dirty(menu_layer_get_layer(s_thread_menu));
    }
  }
  if (detail_window_visible()) {
    if (s_detail_header_layer) {
      layer_mark_dirty(s_detail_header_layer);
    }
    if (s_detail_layer && s_context_loading) {
      layer_mark_dirty(s_detail_layer);
    }
  }
  update_stream_timer();
}

static void update_stream_timer(void) {
  if (!animation_active()) {
    if (s_stream_timer) {
      app_timer_cancel(s_stream_timer);
      s_stream_timer = NULL;
    }
    return;
  }
  if (!s_stream_timer) {
    s_stream_timer = app_timer_register(STREAM_TICK_MS, stream_timer_callback, NULL);
  }
}

static void refresh_timer_callback(void *context) {
  s_refresh_timer = NULL;
  request_refresh();
}

static void schedule_refresh(uint32_t delay_ms) {
  if (s_refresh_timer) {
    app_timer_cancel(s_refresh_timer);
  }
  s_refresh_timer = app_timer_register(delay_ms, refresh_timer_callback, NULL);
}

/* -------------------------------------------------- reference primitives */

/* Pebble has 64 colours and nothing between the glass and the ghost tone, so
   an unlit segment is dithered: a checker of ink over glass reads as the
   half-tone an LCD actually shows when a segment is off. Flat dark grey looks
   like a drawn shape; this looks like a segment that simply is not lit. */
static void fill_ghost(GContext *ctx, GRect r) {
  graphics_context_set_fill_color(ctx, lcd_glass());
  graphics_fill_rect(ctx, r, 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, lcd_dim());
  for (int y = r.origin.y; y < r.origin.y + r.size.h; y++) {
    for (int x = r.origin.x + ((y & 1) ? 1 : 0); x < r.origin.x + r.size.w; x += 2) {
      graphics_draw_pixel(ctx, GPoint(x, y));
    }
  }
}

/* The glass sits below the chassis face, so it carries a shadow along the top
   and left inside edges and catches light along the bottom and right. */
static void draw_panel_bevel(GContext *ctx, GRect panel) {
  graphics_context_set_stroke_color(ctx, lcd_dim());
  graphics_draw_line(ctx, GPoint(panel.origin.x + 2, panel.origin.y + 1),
                     GPoint(panel.origin.x + panel.size.w - 3, panel.origin.y + 1));
  graphics_draw_line(ctx, GPoint(panel.origin.x + 1, panel.origin.y + 2),
                     GPoint(panel.origin.x + 1, panel.origin.y + panel.size.h - 3));
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_line(ctx, GPoint(panel.origin.x + 2, panel.origin.y + panel.size.h - 2),
                     GPoint(panel.origin.x + panel.size.w - 3, panel.origin.y + panel.size.h - 2));
  graphics_draw_line(ctx, GPoint(panel.origin.x + panel.size.w - 2, panel.origin.y + 2),
                     GPoint(panel.origin.x + panel.size.w - 2, panel.origin.y + panel.size.h - 3));
}

/* Legends on the reference are letter-spaced small caps. Pebble offers no
   tracking control, so the string is stepped one glyph at a time. */
static int tracked_width(const char *text, GFont font, int tracking) {
  char glyph[2] = { 0, 0 };
  int width = 0;
  for (const char *c = text; *c; c++) {
    if (*c == ' ') {
      width += 4 + tracking;
      continue;
    }
    glyph[0] = *c;
    GSize size = graphics_text_layout_get_content_size(glyph, font, GRect(0, 0, 40, 20),
                                                       GTextOverflowModeTrailingEllipsis,
                                                       GTextAlignmentLeft);
    width += size.w + tracking;
  }
  return width;
}

static void draw_tracked(GContext *ctx, const char *text, GFont font, GPoint origin, int tracking) {
  char glyph[2] = { 0, 0 };
  int x = origin.x;
  for (const char *c = text; *c; c++) {
    if (*c == ' ') {
      x += 4 + tracking;
      continue;
    }
    glyph[0] = *c;
    GSize size = graphics_text_layout_get_content_size(glyph, font, GRect(0, 0, 40, 20),
                                                       GTextOverflowModeTrailingEllipsis,
                                                       GTextAlignmentLeft);
    graphics_draw_text(ctx, glyph, font, GRect(x, origin.y, size.w + 6, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    x += size.w + tracking;
  }
}

/* The crimson rails bracket the glass and taper toward the buttons, the way
   the reference marks LIGHT / UP ENTER / DOWN. */
static void draw_rail(GContext *ctx, GRect bounds, int y, bool point_up) {
  graphics_context_set_fill_color(ctx, rule_color());
  graphics_fill_rect(ctx, GRect(7, y, bounds.size.w - 14, RAIL_HEIGHT), 0, GCornerNone);
  for (int i = 0; i < 4; i++) {
    int dy = point_up ? -i : i;
    graphics_fill_rect(ctx, GRect(7 - i - 1, y + dy, 1, RAIL_HEIGHT), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(bounds.size.w - 7 + i, y + dy, 1, RAIL_HEIGHT), 0, GCornerNone);
  }
}

/* Seven-segment digit. Unlit segments stay visible in a darker tone, which is
   the detail that makes the reference read as an LCD and not a screen. */
static void draw_seg_digit(GContext *ctx, GRect box, uint8_t mask, GColor on, GColor off) {
  int t = box.size.w >= 18 ? 3 : 2;
  int x = box.origin.x, y = box.origin.y, w = box.size.w, h = box.size.h;
  int half = h / 2;
  GRect segs[7] = {
    GRect(x + t, y, w - 2 * t, t),
    GRect(x + w - t, y + t, t, half - t),
    GRect(x + w - t, y + half, t, half - t),
    GRect(x + t, y + h - t, w - 2 * t, t),
    GRect(x, y + half, t, half - t),
    GRect(x, y + t, t, half - t),
    GRect(x + t, y + half - t / 2, w - 2 * t, t),
  };
  for (int i = 0; i < 7; i++) {
    if (mask & (1 << i)) {
      graphics_context_set_fill_color(ctx, on);
      graphics_fill_rect(ctx, segs[i], 0, GCornerNone);
    } else if (gcolor_equal(off, lcd_glass())) {
      graphics_context_set_fill_color(ctx, off);
      graphics_fill_rect(ctx, segs[i], 0, GCornerNone);
    } else {
      fill_ghost(ctx, segs[i]);
    }
  }
}

static uint8_t seg_for_digit(int digit) {
  static const uint8_t table[10] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,
    SEG_A | SEG_B | SEG_G | SEG_C | SEG_D,
    SEG_F | SEG_G | SEG_B | SEG_C,
    SEG_A | SEG_F | SEG_G | SEG_C | SEG_D,
    SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D,
    SEG_A | SEG_B | SEG_C,
    SEG_ALL,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,
  };
  if (digit < 0 || digit > 9) {
    return 0;
  }
  return table[digit];
}

/* Renders a number as segment digits with the unlit segments showing through,
   exactly like the ghosted 88:88 field on the reference. */
static void draw_seg_number(GContext *ctx, GRect box, int value, int digits, GColor on) {
  int gap = box.size.w >= 40 ? 4 : 2;
  int digit_w = (box.size.w - gap * (digits - 1)) / digits;
  bool leading = true;
  for (int i = 0; i < digits; i++) {
    int place = 1;
    for (int p = 0; p < digits - 1 - i; p++) {
      place *= 10;
    }
    int digit = (value / place) % 10;
    if (digit != 0 || i == digits - 1) {
      leading = false;
    }
    uint8_t mask = (leading && digit == 0) ? 0 : seg_for_digit(digit);
    draw_seg_digit(ctx, GRect(box.origin.x + i * (digit_w + gap), box.origin.y, digit_w, box.size.h),
                   mask, on, lcd_dim());
  }
}

/* A BAT-style segmented meter. The reference spends this on battery; here it
   carries how much of the list is behind you, so the band stays useful. */
static void draw_segment_meter(GContext *ctx, GPoint origin, int height, int filled, int total) {
  int cells = 6;
  int cell_w = 3;
  int gap = 2;
  if (total < 1) {
    total = 1;
  }
  int lit = (filled * cells) / total;
  if (lit < 1 && filled > 0) {
    lit = 1;
  }
  for (int i = 0; i < cells; i++) {
    graphics_context_set_fill_color(ctx, i < lit ? legend() : lcd_dim());
    graphics_fill_rect(ctx, GRect(origin.x + i * (cell_w + gap), origin.y, cell_w, height), 0, GCornerNone);
  }
}

/* The day strip, reused as a host strip: one square per configured machine,
   the one in view filled in. */
static void draw_host_strip(GContext *ctx, GPoint origin, int count, int current) {
  int cell = 5;
  int gap = 3;
  for (int i = 0; i < count && i < MAX_HOSTS; i++) {
    GRect mark = GRect(origin.x + i * (cell + gap), origin.y, cell, cell);
    graphics_context_set_stroke_color(ctx, legend());
    graphics_context_set_fill_color(ctx, legend());
    if (i == current) {
      graphics_fill_rect(ctx, mark, 0, GCornerNone);
    } else {
      graphics_draw_rect(ctx, mark);
    }
  }
}

/* The inset glass: a black chassis margin, a dim outline, then the panel. */
static GRect draw_panel(GContext *ctx, GRect bounds) {
  graphics_context_set_fill_color(ctx, chassis());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  GRect panel = GRect(PANEL_INSET, TOP_CHROME,
                      bounds.size.w - 2 * PANEL_INSET,
                      bounds.size.h - TOP_CHROME - BOTTOM_CHROME);
  graphics_context_set_fill_color(ctx, lcd_glass());
  graphics_fill_rect(ctx, panel, 2, GCornersAll);
  graphics_context_set_stroke_color(ctx, lcd_dim());
  graphics_draw_round_rect(ctx, panel, 2);
  draw_panel_bevel(ctx, panel);
  return panel;
}

/* A boxed sub-field, like the outlined complication on the reference. */
static void draw_field_box(GContext *ctx, GRect box) {
  graphics_context_set_stroke_color(ctx, lcd_dim());
  graphics_draw_round_rect(ctx, box, 2);
}

static void draw_legend_band(GContext *ctx, GRect bounds, const char *left, const char *right) {
  graphics_context_set_text_color(ctx, legend());
  if (left && left[0]) {
    draw_tracked(ctx, left, font_legend(), GPoint(6, -3), 1);
  }
  if (right && right[0]) {
    int w = tracked_width(right, font_legend(), 1);
    draw_tracked(ctx, right, font_legend(), GPoint(bounds.size.w - 6 - w, -3), 1);
  }
}

/* The rail turns into the progress indicator while the phone is working, so
   nothing extra has to appear on screen. */
static void draw_busy_rail(GContext *ctx, GRect bounds, int y) {
  graphics_context_set_fill_color(ctx, chassis());
  graphics_fill_rect(ctx, GRect(0, y - 3, bounds.size.w, RAIL_HEIGHT + 6), 0, GCornerNone);
  int span = bounds.size.w - 14;
  int phase = s_stream_phase % 40;
  int t = phase < 20 ? phase : 40 - phase;
  int eased = (t * t * (60 - 2 * t)) / 4000;
  int width = span / 3;
  int x = 7 + (eased * (span - width)) / 100;
  graphics_context_set_fill_color(ctx, rule_color());
  graphics_fill_rect(ctx, GRect(x, y, width, RAIL_HEIGHT), 0, GCornerNone);
}

/* Micro caption, the size the reference uses for BAT and the day letters. */
static void draw_caption(GContext *ctx, const char *text, GPoint at, GColor color) {
  graphics_context_set_text_color(ctx, color);
  draw_tracked(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_14), at, 1);
}

static void sync_age_text(char *out, size_t size) {
  if (s_last_sync == 0) {
    snprintf(out, size, "--");
    return;
  }
  int secs = (int)(time(NULL) - s_last_sync);
  if (secs < 60) {
    snprintf(out, size, "%ds", secs);
  } else if (secs < 3600) {
    snprintf(out, size, "%dm", secs / 60);
  } else {
    snprintf(out, size, "%dh", secs / 3600);
  }
}

/* ------------------------------------------------------------ state marks */

static GColor state_color(const char *state) {
  if (state_is(state, "needs") || state_is(state, "err") || state_is(state, "offline")) {
    return alert_color();
  }
  if (state_is(state, "run")) {
    return active_color();
  }
  if (state_is(state, "settled") || state_is(state, "snooze") || state_is(state, "empty")) {
    return lcd_dim();
  }
  return lcd_ink();
}

static const char *state_word(const char *state) {
  if (state_is(state, "needs")) return "NEEDS YOU";
  if (state_is(state, "run")) return "RUNNING";
  if (state_is(state, "err")) return "ERROR";
  if (state_is(state, "idle")) return "IDLE";
  if (state_is(state, "settled")) return "ALL SETTLED";
  if (state_is(state, "snooze")) return "SNOOZED";
  if (state_is(state, "offline")) return "NO REPLY";
  return "NO THREADS";
}

static int state_headline_count(const HostItem *host) {
  if (state_is(host->state, "needs")) return host->needs;
  if (state_is(host->state, "run")) return host->run;
  if (state_is(host->state, "idle")) return host->idle;
  if (state_is(host->state, "settled")) return host->settled;
  return 0;
}

/* A square block, not a dot: the reference marks its day strip and battery in
   squares, and shape (filled / hollow / struck) carries the state on the
   monochrome watches where colour cannot. */
static void draw_state_mark(GContext *ctx, GRect box, const char *state, bool inverted) {
  GColor ink = inverted ? lcd_glass() : state_color(state);
  bool hollow = state_is(state, "settled") || state_is(state, "snooze") ||
                state_is(state, "empty") || state_is(state, "offline");

  GRect mark = box;
  if (state_is(state, "run")) {
    int phase = (s_stream_phase / 4) % 8;
    int grow = phase < 4 ? phase : 8 - phase;
    mark = GRect(box.origin.x - grow / 2, box.origin.y - grow / 2,
                 box.size.w + grow, box.size.h + grow);
  }

  graphics_context_set_fill_color(ctx, ink);
  graphics_context_set_stroke_color(ctx, ink);
  if (hollow) {
    graphics_draw_rect(ctx, mark);
    if (state_is(state, "snooze")) {
      graphics_fill_rect(ctx, GRect(mark.origin.x + 2, mark.origin.y + mark.size.h / 2,
                                    mark.size.w - 4, 1), 0, GCornerNone);
    }
  } else {
    graphics_fill_rect(ctx, mark, 0, GCornerNone);
    if (state_is(state, "err")) {
      graphics_context_set_stroke_color(ctx, inverted ? lcd_ink() : lcd_glass());
      graphics_draw_line(ctx, GPoint(mark.origin.x + 1, mark.origin.y + 1),
                         GPoint(mark.origin.x + mark.size.w - 2, mark.origin.y + mark.size.h - 2));
      graphics_draw_line(ctx, GPoint(mark.origin.x + mark.size.w - 2, mark.origin.y + 1),
                         GPoint(mark.origin.x + 1, mark.origin.y + mark.size.h - 2));
    }
  }
}

/* ------------------------------------------------------------ host screen */

/* The home screen is an instrument, not a list: one big segment readout for
   the number that matters on this machine, a boxed field carrying the other
   three counts, and the host strip along the bottom. UP/DOWN steps machines
   the way a mode button steps a watch. */

static void draw_self_test(GContext *ctx, GRect panel, const char *title, const char *hint) {
  /* Nothing to show is still a working instrument: the glass runs its
     all-segments test, which is what a real one does with no data. */
  int digits = 4;
  int digit_w = panel.size.w >= 130 ? 22 : 18;
  int digit_h = 34;
  int gap = 5;
  int total_w = digits * digit_w + (digits - 1) * gap + 6;
  int x = panel.origin.x + (panel.size.w - total_w) / 2;
  int y = panel.origin.y + 12;

  int lit = (s_stream_phase / 6) % (digits + 3);
  for (int i = 0; i < digits; i++) {
    int cx = x + i * (digit_w + gap) + (i >= 2 ? 6 : 0);
    draw_seg_digit(ctx, GRect(cx, y, digit_w, digit_h),
                   i == lit ? SEG_ALL : 0, lcd_ink(), lcd_dim());
  }
  int colon_x = x + 2 * (digit_w + gap) - 1;
  graphics_context_set_fill_color(ctx, lcd_dim());
  graphics_fill_rect(ctx, GRect(colon_x, y + 10, 3, 3), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(colon_x, y + digit_h - 13, 3, 3), 0, GCornerNone);

  graphics_context_set_fill_color(ctx, lcd_dim());
  graphics_fill_rect(ctx, GRect(panel.origin.x + 8, y + digit_h + 10, panel.size.w - 16, 1), 0, GCornerNone);

  graphics_context_set_text_color(ctx, lcd_ink());
  int tw = tracked_width(title, font_legend(), 2);
  draw_tracked(ctx, title, font_legend(),
               GPoint(panel.origin.x + (panel.size.w - tw) / 2, y + digit_h + 14), 2);

  graphics_context_set_text_color(ctx, lcd_dim());
  graphics_draw_text(ctx, hint, font_row_detail(),
                     GRect(panel.origin.x + 6, y + digit_h + 32, panel.size.w - 12, 36),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  /* Even with nothing to show the instrument reports on itself. */
  char age[12];
  sync_age_text(age, sizeof(age));
  char foot[28];
  snprintf(foot, sizeof(foot), "%s   SYN %s", BUILD_LABEL, age);
  int fw = tracked_width(foot, fonts_get_system_font(FONT_KEY_GOTHIC_14), 1);
  draw_caption(ctx, foot, GPoint(panel.origin.x + (panel.size.w - fw) / 2,
                                 panel.origin.y + panel.size.h - 17), lcd_dim());
}

static void host_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GRect panel = draw_panel(ctx, bounds);
  bool busy = s_loading_hosts || !s_hosts_synced;

  char right[20];
  if (busy) {
    snprintf(right, sizeof(right), "SYNC");
  } else if (s_host_count > 0) {
    snprintf(right, sizeof(right), "%d/%d", s_host_cursor + 1, s_host_count);
  } else {
    snprintf(right, sizeof(right), "--");
  }
  draw_legend_band(ctx, bounds, "T3 CODE", right);

  if (busy) {
    draw_busy_rail(ctx, bounds, LEGEND_HEIGHT);
  } else {
    draw_rail(ctx, bounds, LEGEND_HEIGHT, true);
  }
  draw_rail(ctx, bounds, bounds.size.h - BOTTOM_CHROME, false);

  if (!s_hosts_synced) {
    draw_self_test(ctx, panel, "CONNECTING", "Reaching the phone bridge");
    return;
  }
  if (s_host_count == 0) {
    if (s_link_down) {
      draw_self_test(ctx, panel, "NO LINK", "Open the T3 app on your phone");
    } else {
      draw_self_test(ctx, panel, "NO HOSTS",
                     s_status_text[0] ? s_status_text : "Add a server in app settings");
    }
    graphics_context_set_text_color(ctx, legend());
    draw_tracked(ctx, "SELECT TO RETRY", font_legend(),
                 GPoint(bounds.size.w - 6 - tracked_width("SELECT TO RETRY", font_legend(), 1),
                        bounds.size.h - BOTTOM_CHROME + 1), 1);
    return;
  }

  HostItem *host = &s_hosts[clamp_int(s_host_cursor, 0, s_host_count - 1)];
  int inner_x = panel.origin.x + 7;
  int inner_w = panel.size.w - 14;
  int total = host->needs + host->run + host->idle + host->settled;

  /* Field one: the machine, with its size underneath. The reference stacks a
     caption under every field rather than letting a value stand alone. */
  graphics_context_set_text_color(ctx, lcd_ink());
  graphics_draw_text(ctx, host->title, font_row_title(),
                     GRect(inner_x, panel.origin.y + 2, inner_w - 66, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  char sub[28];
  snprintf(sub, sizeof(sub), "%d THREAD%s", total, total == 1 ? "" : "S");
  draw_caption(ctx, sub, GPoint(inner_x, panel.origin.y + 19), lcd_dim());

  /* Field two: the boxed complication, ghosted segments with their own
     captions so the three numbers are readable rather than decorative. */
  GRect field = GRect(panel.origin.x + panel.size.w - 64, panel.origin.y + 3, 57, 33);
  draw_field_box(ctx, field);
  int trio[3] = { host->run, host->idle, host->settled };
  const char *trio_caps[3] = { "R", "I", "S" };
  for (int i = 0; i < 3; i++) {
    draw_seg_number(ctx, GRect(field.origin.x + 6 + i * 17, field.origin.y + 4, 11, 14),
                    clamp_int(trio[i], 0, 99), 1, lcd_ink());
    draw_caption(ctx, trio_caps[i], GPoint(field.origin.x + 9 + i * 17, field.origin.y + 16), lcd_dim());
  }

  /* Field three: the headline. One big segment readout, the way the time
     dominates the reference, with two caption lines beside it. */
  int big_h = clamp_int(panel.size.h - 92, 30, 44);
  int big_w = big_h > 38 ? 26 : 22;
  int big_y = panel.origin.y + 42;
  GColor ink = state_color(host->state);
  if (gcolor_equal(ink, lcd_dim())) {
    ink = lcd_ink();
  }
  draw_seg_number(ctx, GRect(inner_x, big_y, big_w * 2 + 6, big_h),
                  clamp_int(state_headline_count(host), 0, 99), 2, ink);

  int label_x = inner_x + big_w * 2 + 14;
  graphics_context_set_text_color(ctx, lcd_ink());
  draw_tracked(ctx, state_word(host->state), font_legend(), GPoint(label_x, big_y + 2), 1);
  char of_line[24];
  snprintf(of_line, sizeof(of_line), "OF %d", total);
  draw_caption(ctx, of_line, GPoint(label_x, big_y + 17), lcd_dim());
  draw_caption(ctx, state_is(host->state, "offline") ? "TAP RETRY" : "TAP OPEN",
               GPoint(label_x, big_y + 30), lcd_dim());

  if (state_is_live(host->state) || state_is(host->state, "needs")) {
    draw_state_mark(ctx, GRect(panel.origin.x + panel.size.w - 15, big_y + 4, 7, 7), host->state, false);
  }

  /* The indicator row: hairline, meter with its caption, sync age, and the
     day strip carrying which machine is in view. */
  int strip_y = panel.origin.y + panel.size.h - 15;
  graphics_context_set_fill_color(ctx, lcd_dim());
  graphics_fill_rect(ctx, GRect(inner_x, strip_y - 5, inner_w, 1), 0, GCornerNone);

  draw_caption(ctx, "ACT", GPoint(inner_x, strip_y - 4), lcd_dim());
  draw_segment_meter(ctx, GPoint(inner_x + 24, strip_y + 2), 7, total - host->settled, total);

  char age[12];
  sync_age_text(age, sizeof(age));
  char sync_line[20];
  snprintf(sync_line, sizeof(sync_line), "SYN %s", age);
  draw_caption(ctx, sync_line, GPoint(inner_x + 62, strip_y - 4), lcd_dim());

  draw_host_strip(ctx, GPoint(panel.origin.x + panel.size.w - 8 - s_host_count * 8, strip_y + 1),
                  s_host_count, s_host_cursor);

  graphics_context_set_text_color(ctx, legend());
  draw_tracked(ctx, "HOST", font_legend(), GPoint(6, bounds.size.h - BOTTOM_CHROME + 1), 1);
  const char *hint = "OPEN";
  int hw = tracked_width(hint, font_legend(), 1);
  draw_tracked(ctx, hint, font_legend(), GPoint(bounds.size.w - 6 - hw, bounds.size.h - BOTTOM_CHROME + 1), 1);
}

static void host_step(int delta) {
  if (s_host_count <= 0) {
    return;
  }
  s_host_cursor = (s_host_cursor + delta + s_host_count) % s_host_count;
  layer_mark_dirty(s_host_layer);
}

static void host_up_click(ClickRecognizerRef recognizer, void *context) {
  host_step(-1);
}

static void host_down_click(ClickRecognizerRef recognizer, void *context) {
  host_step(1);
}

static void host_select_click(ClickRecognizerRef recognizer, void *context) {
  if (!s_hosts_synced || s_host_count == 0) {
    request_refresh();
    return;
  }
  open_selected_host();
}

static void host_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, host_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, host_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, host_select_click);
}

static void open_selected_host(void) {
  s_selected_host_index = clamp_int(s_host_cursor, 0, s_host_count - 1);
  s_session_count = 0;
  s_project_count = 0;
  s_threads_synced = false;
  s_loading_threads = true;
  window_stack_push(s_thread_window, true);
  update_stream_timer();

  DictionaryIterator *iter = NULL;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK || !iter) {
    set_status("Phone link busy");
    return;
  }
  int command = CMD_SELECT_HOST;
  dict_write_int(iter, KEY_CMD, &command, sizeof(command), true);
  dict_write_cstring(iter, KEY_HOST_ID, s_hosts[s_selected_host_index].id);
  app_message_outbox_send();
}

/* ---------------------------------------------------------- thread screen */

static void draw_ghost_row(GContext *ctx, GRect bounds, int row) {
  int lit = (s_stream_phase / 3) % (GHOST_ROWS + 2);
  GColor tone = (row == lit) ? lcd_ink() : lcd_dim();
  int widths[GHOST_ROWS] = { 74, 96, 62, 84 };
  int width = widths[row % GHOST_ROWS];
  if (width > bounds.size.w - 26) {
    width = bounds.size.w - 26;
  }

  graphics_context_set_fill_color(ctx, lcd_glass());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, tone);
  graphics_fill_rect(ctx, GRect(6, bounds.size.h / 2 - 9, 7, 7), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(19, bounds.size.h / 2 - 9, width, 8), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, lcd_dim());
  graphics_fill_rect(ctx, GRect(19, bounds.size.h / 2 + 3, width / 2, 5), 0, GCornerNone);
}

static void draw_list_row(GContext *ctx, const Layer *cell_layer, bool selected,
                          const char *title, const char *detail, const char *state) {
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_fill_color(ctx, selected ? lcd_ink() : lcd_glass());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  draw_state_mark(ctx, GRect(6, bounds.size.h / 2 - 9, 7, 7), state, selected);

  graphics_context_set_text_color(ctx, selected ? lcd_glass() : lcd_ink());
  graphics_draw_text(ctx, title, font_row_title(),
                     GRect(19, 0, bounds.size.w - 25, 21),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  graphics_context_set_text_color(ctx, selected ? lcd_glass() : lcd_dim());
  graphics_draw_text(ctx, detail, font_row_detail(),
                     GRect(19, 19, bounds.size.w - 25, 17),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  graphics_context_set_fill_color(ctx, selected ? lcd_glass() : lcd_dim());
  graphics_fill_rect(ctx, GRect(6, bounds.size.h - 1, bounds.size.w - 12, 1), 0, GCornerNone);
}

static void thread_chrome_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  const char *host = (s_selected_host_index >= 0 && s_selected_host_index < s_host_count)
    ? s_hosts[s_selected_host_index].title : "HOST";
  bool busy = s_loading_threads || !s_threads_synced;

  graphics_context_set_fill_color(ctx, chassis());
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, TOP_CHROME), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(0, bounds.size.h - BOTTOM_CHROME, bounds.size.w, BOTTOM_CHROME), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(0, TOP_CHROME, PANEL_INSET, bounds.size.h - TOP_CHROME - BOTTOM_CHROME), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(bounds.size.w - PANEL_INSET, TOP_CHROME, PANEL_INSET,
                                bounds.size.h - TOP_CHROME - BOTTOM_CHROME), 0, GCornerNone);

  char right[20];
  if (busy) {
    snprintf(right, sizeof(right), "SYNC");
  } else {
    snprintf(right, sizeof(right), "%d", s_session_count);
  }
  draw_legend_band(ctx, bounds, host, right);

  if (busy) {
    draw_busy_rail(ctx, bounds, LEGEND_HEIGHT);
  } else {
    draw_rail(ctx, bounds, LEGEND_HEIGHT, true);
  }
  draw_rail(ctx, bounds, bounds.size.h - BOTTOM_CHROME, false);

  int needs = 0, running = 0, settled = 0;
  for (int i = 0; i < s_session_count; i++) {
    if (state_is(s_sessions[i].state, "needs")) needs++;
    else if (state_is(s_sessions[i].state, "run")) running++;
    else if (state_is(s_sessions[i].state, "settled") || state_is(s_sessions[i].state, "snooze")) settled++;
  }
  char tally[24];
  snprintf(tally, sizeof(tally), "N%d R%d S%d", needs, running, settled);
  graphics_context_set_text_color(ctx, legend());
  draw_tracked(ctx, tally, font_legend(), GPoint(6, bounds.size.h - BOTTOM_CHROME + 1), 1);
  draw_host_strip(ctx, GPoint(bounds.size.w - 8 - s_host_count * 8, bounds.size.h - BOTTOM_CHROME + 5),
                  s_host_count, s_selected_host_index);
}

static uint16_t thread_num_sections(MenuLayer *menu_layer, void *data) {
  return 2;
}

static uint16_t thread_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  if (section_index == 1) {
    return (!s_threads_synced || s_loading_threads) ? 0 : s_project_count;
  }
  if (!s_threads_synced || s_loading_threads) {
    return GHOST_ROWS;
  }
  return s_session_count > 0 ? s_session_count : 1;
}

static int16_t thread_header_height(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  if (section_index == 1 && s_threads_synced && s_project_count > 0) {
    return SECTION_HEADER_HEIGHT;
  }
  return 0;
}

static void thread_draw_header(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *data) {
  if (section_index != 1) {
    return;
  }
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_fill_color(ctx, lcd_ink());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, lcd_glass());
  draw_tracked(ctx, "START NEW", font_legend(), GPoint(6, -4), 1);
}

static int16_t thread_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  return ROW_HEIGHT;
}

static void thread_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
  bool selected = menu_layer_is_index_selected(s_thread_menu, cell_index);

  if (cell_index->section == 1) {
    if (cell_index->row >= s_project_count) {
      return;
    }
    ProjectItem *project = &s_projects[cell_index->row];
    draw_list_row(ctx, cell_layer, selected, project->title, project->directory, "idle");
    return;
  }

  if (!s_threads_synced || s_loading_threads) {
    draw_ghost_row(ctx, layer_get_bounds(cell_layer), cell_index->row);
    return;
  }
  if (s_session_count == 0) {
    draw_list_row(ctx, cell_layer, selected, "No threads", "nothing open here", "empty");
    return;
  }
  if (cell_index->row >= s_session_count) {
    return;
  }
  SessionItem *item = &s_sessions[cell_index->row];
  draw_list_row(ctx, cell_layer, selected, item->title, item->detail, item->state);
}

static void thread_select(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  if (cell_index->section == 1) {
    if (cell_index->row >= s_project_count) {
      return;
    }
    s_selected_project_index = cell_index->row;
    s_dictation_target = DictationTargetProject;
    set_status("Dictate prompt");
    start_dictation();
    return;
  }

  if (!s_threads_synced || s_session_count == 0 || cell_index->row >= s_session_count) {
    return;
  }
  s_selected_index = cell_index->row;
  s_showing_context = false;
  s_context_loading = false;
  s_context_page_nav = false;
  s_pending_context_session_id[0] = '\0';
  s_pending_context_request_id[0] = '\0';
  s_full_context[0] = '\0';
  window_stack_push(s_detail_window, true);
  update_detail_text();
  request_detail(s_selected_index, false);
}

/* ----------------------------------------------------------------- outbox */

static void send_command_begin(DictionaryIterator **iter, int command) {
  if (app_message_outbox_begin(iter) != APP_MSG_OK || !*iter) {
    set_status("Phone link busy");
    return;
  }
  dict_write_int(*iter, KEY_CMD, &command, sizeof(command), true);
}

static void request_refresh(void) {
  if (s_loading_hosts) {
    return;
  }
  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_REFRESH);
  if (!iter) {
    return;
  }
  s_loading_hosts = true;
  mark_all_dirty();
  update_stream_timer();
  app_message_outbox_send();
}

static void request_detail(int index, bool full_context) {
  if (index < 0 || index >= s_session_count) {
    return;
  }
  s_selected_index = index;
  if (full_context) {
    request_context_page(-1);
    return;
  }
  s_showing_context = false;
  s_context_loading = false;
  s_context_page_nav = false;
  s_pending_context_session_id[0] = '\0';
  s_pending_context_request_id[0] = '\0';
  s_context_page = 0;
  s_context_page_count = 1;
  s_full_context[0] = '\0';
  update_detail_text();

  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_DETAIL);
  if (!iter) {
    return;
  }
  dict_write_int(iter, KEY_INDEX, &index, sizeof(index), true);
  dict_write_cstring(iter, KEY_SESSION_ID, s_sessions[index].id);
  app_message_outbox_send();
}

static void request_context_page(int page) {
  if (s_selected_index < 0 || s_selected_index >= s_session_count || s_context_loading) {
    return;
  }
  if (page < -1) {
    page = 0;
  }
  if (s_context_page_count > 0 && page >= s_context_page_count) {
    page = s_context_page_count - 1;
  }

  s_showing_context = true;
  s_context_loading = true;
  s_pending_context_page = page;
  strncpy(s_pending_context_session_id, s_sessions[s_selected_index].id, sizeof(s_pending_context_session_id) - 1);
  s_pending_context_session_id[sizeof(s_pending_context_session_id) - 1] = '\0';
  s_context_request_counter = (s_context_request_counter % 9998) + 1;
  snprintf(s_pending_context_request_id, sizeof(s_pending_context_request_id), "c%d", s_context_request_counter);
  s_full_context[0] = '\0';
  update_detail_text();
  update_stream_timer();

  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_CONTEXT);
  if (!iter) {
    detail_exit_context(false);
    return;
  }
  dict_write_int(iter, KEY_INDEX, &s_selected_index, sizeof(s_selected_index), true);
  dict_write_int(iter, KEY_CONTEXT_PAGE, &page, sizeof(page), true);
  dict_write_cstring(iter, KEY_SESSION_ID, s_sessions[s_selected_index].id);
  dict_write_cstring(iter, KEY_REQUEST_ID, s_pending_context_request_id);
  app_message_outbox_send();
}

static void send_prompt_text(const char *text) {
  if (s_selected_index < 0 || s_selected_index >= s_session_count || !text || !text[0]) {
    return;
  }
  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_PROMPT);
  if (!iter) {
    return;
  }
  dict_write_int(iter, KEY_INDEX, &s_selected_index, sizeof(s_selected_index), true);
  dict_write_cstring(iter, KEY_SESSION_ID, s_sessions[s_selected_index].id);
  dict_write_cstring(iter, KEY_REQUEST_ID, s_sessions[s_selected_index].request_id);
  dict_write_cstring(iter, KEY_REQUEST_KIND, s_sessions[s_selected_index].request_kind);
  dict_write_cstring(iter, KEY_PROMPT, text);
  s_sending_message = true;
  set_status("Sending");
  update_stream_timer();
  app_message_outbox_send();
}

static void send_new_thread_text(const char *text) {
  if (s_selected_project_index < 0 || s_selected_project_index >= s_project_count || !text || !text[0]) {
    return;
  }
  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_NEW_THREAD);
  if (!iter) {
    return;
  }
  dict_write_int(iter, KEY_INDEX, &s_selected_project_index, sizeof(s_selected_project_index), true);
  dict_write_cstring(iter, KEY_PROJECT_ID, s_projects[s_selected_project_index].id);
  dict_write_cstring(iter, KEY_PROMPT, text);
  s_sending_message = true;
  set_status("Starting");
  update_stream_timer();
  app_message_outbox_send();
}

/* ------------------------------------------------------- transcript pages */

static int role_body_start(const char *block, int block_len) {
  for (int i = 0; i < block_len; i++) {
    if (block[i] == '\n') {
      return i + 1;
    }
  }
  return 0;
}

static void block_role(char *dest, size_t size, const char *block, int block_len) {
  int len = role_body_start(block, block_len);
  if (len > 0) {
    len -= 1;
  }
  if (len <= 0 || len >= (int)size) {
    len = 0;
  }
  if (len > 0) {
    memcpy(dest, block, len);
  }
  dest[len] = '\0';
}

static int next_context_block_len(const char *text, int text_len, int start, int *block_start, int *block_len) {
  int len = text_len;
  while (start < len && text[start] == '\n') {
    start++;
  }
  if (start >= len) {
    return len;
  }
  int end = start;
  while (end < len) {
    if (text[end] == '\n' && end + 1 < len && text[end + 1] == '\n') {
      break;
    }
    end++;
  }
  *block_start = start;
  *block_len = end - start;
  return end + 2;
}

static int text_height(const char *text, GFont font, int width) {
  GSize size = graphics_text_layout_get_content_size(
    text, font, GRect(0, 0, width, 1000), GTextOverflowModeWordWrap, GTextAlignmentLeft);
  return size.h;
}

static int context_card_height(const char *body, int width) {
  int body_h = text_height(body, font_body(), width - 20);
  return clamp_int(20 + body_h + 10, 52, 260);
}

static int context_content_height(int width) {
  if (!s_full_context[0] || s_context_loading) {
    return DETAIL_CONTENT_MIN_HEIGHT;
  }
  int y = CARD_PAD;
  int pos = 0;
  int block_start = 0;
  int block_len = 0;
  int context_len = strlen(s_full_context);
  while (pos < context_len) {
    pos = next_context_block_len(s_full_context, context_len, pos, &block_start, &block_len);
    if (block_len <= 0) {
      continue;
    }
    int body_start = role_body_start(s_full_context + block_start, block_len);
    int body_len = block_len - body_start;
    if (body_len >= (int)sizeof(s_draw_body)) {
      body_len = sizeof(s_draw_body) - 1;
    }
    memcpy(s_draw_body, s_full_context + block_start + body_start, body_len);
    s_draw_body[body_len] = '\0';
    y += context_card_height(s_draw_body, width) + 6;
  }
  return clamp_int(y + CARD_PAD, DETAIL_CONTENT_MIN_HEIGHT, DETAIL_CONTENT_HEIGHT);
}

static int detail_content_height_for_width(int width) {
  if (s_showing_context) {
    return context_content_height(width);
  }
  if (s_selected_index < 0 || s_selected_index >= s_session_count) {
    return DETAIL_CONTENT_MIN_HEIGHT;
  }
  SessionItem *item = &s_sessions[s_selected_index];
  const char *summary = item->summary[0] ? item->summary : "No summary yet.";
  int height = 24 + text_height(summary, font_body(), width - 20) + 48;
  return clamp_int(height, DETAIL_CONTENT_MIN_HEIGHT, DETAIL_CONTENT_HEIGHT);
}

static void update_detail_text(void) {
  if (s_detail_layer && s_scroll_layer) {
    GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
    int width = scroll_bounds.size.w;
    int height = detail_content_height_for_width(width);
    if (height < scroll_bounds.size.h) {
      height = scroll_bounds.size.h;
    }
    layer_set_frame(s_detail_layer, GRect(0, 0, width, height));
    scroll_layer_set_content_size(s_scroll_layer, GSize(width, height));
    layer_mark_dirty(s_detail_layer);
  }
  if (s_detail_header_layer) {
    layer_mark_dirty(s_detail_header_layer);
  }
}

static int draw_context_card(GContext *ctx, int y, int width, const char *role, const char *body, int card_h) {
  bool from_user = strcmp(role, "You") == 0;
  graphics_context_set_fill_color(ctx, from_user ? lcd_ink() : lcd_dim());
  graphics_fill_rect(ctx, GRect(6, y + 2, 2, card_h - 8), 0, GCornerNone);

  graphics_context_set_text_color(ctx, lcd_dim());
  draw_tracked(ctx, from_user ? "YOU" : "AGENT", font_legend(), GPoint(13, y - 4), 1);

  graphics_context_set_text_color(ctx, lcd_ink());
  graphics_draw_text(ctx, body, font_body(), GRect(13, y + 12, width - 20, card_h - 14),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  return card_h;
}

static void draw_context_page(GContext *ctx, GRect bounds) {
  if (s_context_loading) {
    for (int i = 0; i < 3; i++) {
      int lit = (s_stream_phase / 3) % 4;
      graphics_context_set_fill_color(ctx, i == lit ? lcd_ink() : lcd_dim());
      int w = 60 + i * 30;
      if (w > bounds.size.w - 26) {
        w = bounds.size.w - 26;
      }
      graphics_fill_rect(ctx, GRect(13, 14 + i * 16, w, 8), 0, GCornerNone);
    }
    return;
  }

  int y = CARD_PAD;
  int pos = 0;
  int block_start = 0;
  int block_len = 0;
  int context_len = strlen(s_full_context);
  char role[16];
  while (pos < context_len) {
    pos = next_context_block_len(s_full_context, context_len, pos, &block_start, &block_len);
    if (block_len <= 0) {
      continue;
    }
    block_role(role, sizeof(role), s_full_context + block_start, block_len);
    int body_start = role_body_start(s_full_context + block_start, block_len);
    int body_len = block_len - body_start;
    if (body_len >= (int)sizeof(s_draw_body)) {
      body_len = sizeof(s_draw_body) - 1;
    }
    memcpy(s_draw_body, s_full_context + block_start + body_start, body_len);
    s_draw_body[body_len] = '\0';
    int card_h = context_card_height(s_draw_body, bounds.size.w);
    y += draw_context_card(ctx, y, bounds.size.w, role, s_draw_body, card_h) + 6;
  }
}

static void detail_content_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, lcd_glass());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (s_selected_index < 0 || s_selected_index >= s_session_count) {
    return;
  }
  if (s_showing_context) {
    draw_context_page(ctx, bounds);
    return;
  }

  SessionItem *item = &s_sessions[s_selected_index];
  const char *summary = item->summary[0] ? item->summary : "No summary yet.";
  graphics_context_set_text_color(ctx, lcd_dim());
  draw_tracked(ctx, "LATEST", font_legend(), GPoint(10, -2), 1);
  int lw = tracked_width(item->detail, fonts_get_system_font(FONT_KEY_GOTHIC_14), 1);
  draw_caption(ctx, item->detail, GPoint(bounds.size.w - 10 - lw, -1), lcd_dim());
  graphics_context_set_fill_color(ctx, lcd_dim());
  graphics_fill_rect(ctx, GRect(10, 16, bounds.size.w - 20, 1), 0, GCornerNone);
  graphics_context_set_text_color(ctx, lcd_ink());
  graphics_draw_text(ctx, summary, font_body(), GRect(10, 20, bounds.size.w - 20, bounds.size.h - 44),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  draw_caption(ctx, s_showing_context ? "BACK SUMMARY" : "SELECT FULL LOG   HOLD SPEAK",
               GPoint(10, bounds.size.h - 20), lcd_dim());
}

static void detail_header_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, chassis());
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, TOP_CHROME), 0, GCornerNone);

  if (s_selected_index < 0 || s_selected_index >= s_session_count) {
    draw_legend_band(ctx, bounds, "THREAD", "");
    draw_rail(ctx, bounds, LEGEND_HEIGHT, true);
    return;
  }
  SessionItem *item = &s_sessions[s_selected_index];
  char right[20];
  if (s_sending_message) {
    snprintf(right, sizeof(right), "SEND");
  } else if (s_showing_context) {
    snprintf(right, sizeof(right), "%d/%d", s_context_page + 1, s_context_page_count);
  } else {
    snprintf(right, sizeof(right), "%s", state_word(item->state));
  }
  draw_legend_band(ctx, bounds, item->title, right);
  if (s_context_loading || s_sending_message) {
    draw_busy_rail(ctx, bounds, LEGEND_HEIGHT);
  } else {
    draw_rail(ctx, bounds, LEGEND_HEIGHT, true);
  }
}

/* ----------------------------------------------------------- detail input */

static void detail_reset_scroll(bool animated) {
  if (s_scroll_layer) {
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, 0), animated);
  }
}

static void detail_exit_context(bool animated) {
  s_showing_context = false;
  s_context_loading = false;
  s_context_page_nav = false;
  s_context_page = 0;
  s_context_page_count = 1;
  s_full_context[0] = '\0';
  detail_reset_scroll(false);
  update_detail_text();
  update_stream_timer();
}

static void detail_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_showing_context) {
    s_context_page_nav = !s_context_page_nav;
    update_detail_text();
  } else {
    request_detail(s_selected_index, true);
  }
}

static void detail_select_long_handler(ClickRecognizerRef recognizer, void *context) {
  s_dictation_target = DictationTargetSession;
  start_dictation();
}

static void detail_back_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_showing_context || s_context_loading) {
    detail_exit_context(false);
    return;
  }
  window_stack_pop(true);
}

static int detail_scroll_max_offset(void) {
  if (!s_scroll_layer || !s_detail_layer) {
    return 0;
  }
  GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  int content_h = layer_get_frame(s_detail_layer).size.h;
  int max_offset = content_h - scroll_bounds.size.h;
  return max_offset > 0 ? max_offset : 0;
}

static void detail_scroll_by(int delta) {
  if (!s_scroll_layer) {
    return;
  }
  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  int next = clamp_int(-offset.y + delta, 0, detail_scroll_max_offset());
  scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, -next), true);
}

static void detail_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_showing_context && s_context_page_nav) {
    request_context_page(s_context_page - 1);
    return;
  }
  detail_scroll_by(-40);
}

static void detail_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_showing_context && s_context_page_nav) {
    request_context_page(s_context_page + 1);
    return;
  }
  detail_scroll_by(40);
}

static void detail_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, detail_select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, detail_select_long_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, detail_back_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, detail_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, detail_down_click_handler);
}

static void dictation_callback(DictationSession *session, DictationSessionStatus status, char *transcription, void *context) {
  if (status == DictationSessionStatusSuccess && transcription && transcription[0]) {
    if (s_dictation_target == DictationTargetProject) {
      send_new_thread_text(transcription);
    } else {
      send_prompt_text(transcription);
    }
  } else {
    set_status("Dictation failed");
  }
}

static void start_dictation(void) {
#ifdef PBL_MICROPHONE
  if (s_dictation) {
    dictation_session_destroy(s_dictation);
    s_dictation = NULL;
  }
  s_dictation = dictation_session_create(512, dictation_callback, NULL);
  if (!s_dictation) {
    set_status("No dictation");
    return;
  }
  dictation_session_start(s_dictation);
#else
  set_status("Voice unavailable");
#endif
}

/* ------------------------------------------------------------------ inbox */

static void reset_hosts(void) {
  memset(s_hosts, 0, sizeof(s_hosts));
  s_host_count = 0;
}

static void reset_sessions(void) {
  memset(s_sessions, 0, sizeof(s_sessions));
  s_session_count = 0;
}

static void reset_projects(void) {
  memset(s_projects, 0, sizeof(s_projects));
  s_project_count = 0;
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  int command = int_tuple(iter, KEY_CMD, 0);

  if (command == CMD_HOST_ITEM) {
    int index = int_tuple(iter, KEY_INDEX, -1);
    if (index == 0) {
      reset_hosts();
    }
    if (index >= 0 && index < MAX_HOSTS) {
      HostItem *host = &s_hosts[index];
      copy_tuple(host->id, sizeof(host->id), iter, KEY_HOST_ID);
      copy_tuple(host->title, sizeof(host->title), iter, KEY_TITLE);
      copy_tuple(host->state, sizeof(host->state), iter, KEY_STATE);
      host->needs = int_tuple(iter, KEY_C_NEEDS, 0);
      host->run = int_tuple(iter, KEY_C_RUN, 0);
      host->idle = int_tuple(iter, KEY_C_IDLE, 0);
      host->settled = int_tuple(iter, KEY_C_SETTLED, 0);
      if (index + 1 > s_host_count) {
        s_host_count = index + 1;
      }
    }
  } else if (command == CMD_HOST_END) {
    int total = int_tuple(iter, KEY_TOTAL, s_host_count);
    if (total == 0) {
      reset_hosts();
    }
    s_loading_hosts = false;
    s_hosts_synced = true;
    s_link_down = false;
    s_last_sync = time(NULL);
    s_host_cursor = clamp_int(s_host_cursor, 0, s_host_count > 0 ? s_host_count - 1 : 0);
    mark_all_dirty();
    update_stream_timer();
    schedule_refresh(REFRESH_INTERVAL_MS);
  } else if (command == CMD_SESSION_ITEM) {
    int index = int_tuple(iter, KEY_INDEX, -1);
    if (index == 0) {
      reset_sessions();
    }
    if (index >= 0 && index < MAX_SESSIONS) {
      SessionItem *item = &s_sessions[index];
      copy_tuple(item->id, sizeof(item->id), iter, KEY_SESSION_ID);
      copy_tuple(item->title, sizeof(item->title), iter, KEY_TITLE);
      copy_tuple(item->detail, sizeof(item->detail), iter, KEY_DETAIL);
      copy_tuple(item->state, sizeof(item->state), iter, KEY_STATE);
      copy_tuple(item->summary, sizeof(item->summary), iter, KEY_SUMMARY);
      copy_tuple(item->request_id, sizeof(item->request_id), iter, KEY_REQUEST_ID);
      copy_tuple(item->request_kind, sizeof(item->request_kind), iter, KEY_REQUEST_KIND);
      if (index + 1 > s_session_count) {
        s_session_count = index + 1;
      }
    }
  } else if (command == CMD_SESSION_END) {
    int total = int_tuple(iter, KEY_TOTAL, s_session_count);
    if (total == 0) {
      reset_sessions();
    }
    s_loading_threads = false;
    s_threads_synced = true;
    mark_all_dirty();
    update_stream_timer();
  } else if (command == CMD_PROJECT_ITEM) {
    int index = int_tuple(iter, KEY_INDEX, -1);
    if (index == 0) {
      reset_projects();
    }
    if (index >= 0 && index < MAX_PROJECTS) {
      ProjectItem *project = &s_projects[index];
      copy_tuple(project->id, sizeof(project->id), iter, KEY_PROJECT_ID);
      copy_tuple(project->title, sizeof(project->title), iter, KEY_TITLE);
      copy_tuple(project->directory, sizeof(project->directory), iter, KEY_DIRECTORY);
      if (index + 1 > s_project_count) {
        s_project_count = index + 1;
      }
    }
  } else if (command == CMD_PROJECT_END) {
    int total = int_tuple(iter, KEY_TOTAL, s_project_count);
    if (total == 0) {
      reset_projects();
    }
    mark_all_dirty();
  } else if (command == CMD_DETAIL) {
    int index = int_tuple(iter, KEY_INDEX, s_selected_index);
    if (index >= 0 && index < MAX_SESSIONS) {
      SessionItem *item = &s_sessions[index];
      copy_tuple(item->title, sizeof(item->title), iter, KEY_TITLE);
      copy_tuple(item->detail, sizeof(item->detail), iter, KEY_DETAIL);
      copy_tuple(item->state, sizeof(item->state), iter, KEY_STATE);
      copy_tuple(item->summary, sizeof(item->summary), iter, KEY_SUMMARY);
      copy_tuple(item->request_id, sizeof(item->request_id), iter, KEY_REQUEST_ID);
      copy_tuple(item->request_kind, sizeof(item->request_kind), iter, KEY_REQUEST_KIND);
      if (index + 1 > s_session_count) {
        s_session_count = index + 1;
      }
    }
    update_detail_text();
    mark_all_dirty();
    update_stream_timer();
  } else if (command == CMD_CONTEXT) {
    char request_id[16] = "";
    char session_id[80] = "";
    copy_tuple(request_id, sizeof(request_id), iter, KEY_REQUEST_ID);
    copy_tuple(session_id, sizeof(session_id), iter, KEY_SESSION_ID);
    if (s_pending_context_request_id[0] && strcmp(request_id, s_pending_context_request_id) != 0) {
      return;
    }
    if (s_pending_context_session_id[0] && session_id[0] &&
        strcmp(session_id, s_pending_context_session_id) != 0) {
      return;
    }
    copy_tuple(s_full_context, sizeof(s_full_context), iter, KEY_CONTEXT);
    s_context_page_count = clamp_int(int_tuple(iter, KEY_TOTAL, 1), 1, 999);
    s_context_page = clamp_int(int_tuple(iter, KEY_CONTEXT_PAGE, s_pending_context_page < 0 ? 0 : s_pending_context_page),
                               0, s_context_page_count - 1);
    s_context_loading = false;
    s_showing_context = true;
    detail_reset_scroll(false);
    update_detail_text();
    update_stream_timer();
  } else if (command == CMD_PROMPT) {
    s_sending_message = false;
    set_status("Sent");
    update_detail_text();
    update_stream_timer();
    schedule_refresh(2500);
  } else if (command == CMD_STATUS) {
    char status[40] = "";
    copy_tuple(status, sizeof(status), iter, KEY_STATUS);
    set_status(status);
  } else if (command == CMD_ERROR) {
    char error[40] = "";
    copy_tuple(error, sizeof(error), iter, KEY_ERROR);
    s_loading_hosts = false;
    s_loading_threads = false;
    s_sending_message = false;
    s_context_loading = false;
    s_hosts_synced = true;
    s_threads_synced = true;
    set_status(error[0] ? error : "Error");
    update_detail_text();
    update_stream_timer();
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  set_status("Dropped a message");
}

static void outbox_failed_callback(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  s_loading_hosts = false;
  s_loading_threads = false;
  s_sending_message = false;
  s_hosts_synced = true;
  s_link_down = true;
  set_status("Phone bridge not responding");
  update_stream_timer();
  mark_all_dirty();
}

/* ---------------------------------------------------------------- windows */

static void host_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  window_set_background_color(window, chassis());

  s_host_layer = layer_create(bounds);
  layer_set_update_proc(s_host_layer, host_layer_update_proc);
  layer_add_child(window_layer, s_host_layer);
  window_set_click_config_provider(window, host_click_config);
}

static void host_window_unload(Window *window) {
  if (s_host_layer) {
    layer_destroy(s_host_layer);
  }
  s_host_layer = NULL;
}

static void thread_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  window_set_background_color(window, chassis());

  GRect list = GRect(PANEL_INSET, TOP_CHROME,
                     bounds.size.w - 2 * PANEL_INSET,
                     bounds.size.h - TOP_CHROME - BOTTOM_CHROME);
  s_thread_menu = menu_layer_create(list);
  menu_layer_set_normal_colors(s_thread_menu, lcd_glass(), lcd_ink());
  menu_layer_set_highlight_colors(s_thread_menu, lcd_ink(), lcd_glass());
  menu_layer_set_callbacks(s_thread_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = thread_num_sections,
    .get_num_rows = thread_num_rows,
    .get_header_height = thread_header_height,
    .get_cell_height = thread_cell_height,
    .draw_header = thread_draw_header,
    .draw_row = thread_draw_row,
    .select_click = thread_select
  });
  menu_layer_set_click_config_onto_window(s_thread_menu, window);
  layer_add_child(window_layer, menu_layer_get_layer(s_thread_menu));

  s_thread_chrome = layer_create(bounds);
  layer_set_update_proc(s_thread_chrome, thread_chrome_update_proc);
  layer_add_child(window_layer, s_thread_chrome);
}

static void thread_window_unload(Window *window) {
  if (s_thread_menu) {
    menu_layer_destroy(s_thread_menu);
  }
  if (s_thread_chrome) {
    layer_destroy(s_thread_chrome);
  }
  s_thread_menu = NULL;
  s_thread_chrome = NULL;
}

static void detail_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  window_set_background_color(window, chassis());

  GRect glass = GRect(PANEL_INSET, TOP_CHROME,
                      bounds.size.w - 2 * PANEL_INSET,
                      bounds.size.h - TOP_CHROME - 2);
  s_scroll_layer = scroll_layer_create(glass);
  if (!s_scroll_layer) {
    return;
  }
  layer_add_child(window_layer, scroll_layer_get_layer(s_scroll_layer));
  scroll_layer_set_content_size(s_scroll_layer, GSize(glass.size.w, DETAIL_CONTENT_HEIGHT));

  s_detail_layer = layer_create(GRect(0, 0, glass.size.w, DETAIL_CONTENT_HEIGHT));
  if (!s_detail_layer) {
    return;
  }
  layer_set_update_proc(s_detail_layer, detail_content_update_proc);
  scroll_layer_add_child(s_scroll_layer, s_detail_layer);

  s_detail_header_layer = layer_create(GRect(0, 0, bounds.size.w, TOP_CHROME));
  if (!s_detail_header_layer) {
    return;
  }
  layer_set_update_proc(s_detail_header_layer, detail_header_update_proc);
  layer_add_child(window_layer, s_detail_header_layer);

  window_set_click_config_provider(window, detail_click_config_provider);
  update_detail_text();
}

static void detail_window_unload(Window *window) {
  if (s_detail_layer) {
    layer_destroy(s_detail_layer);
  }
  if (s_scroll_layer) {
    scroll_layer_destroy(s_scroll_layer);
  }
  if (s_detail_header_layer) {
    layer_destroy(s_detail_header_layer);
  }
  s_detail_layer = NULL;
  s_scroll_layer = NULL;
  s_detail_header_layer = NULL;
}

static void init(void) {
  reset_hosts();
  reset_sessions();
  reset_projects();
  set_status(BUILD_LABEL);

  s_host_window = window_create();
  window_set_window_handlers(s_host_window, (WindowHandlers) {
    .load = host_window_load,
    .unload = host_window_unload
  });

  s_thread_window = window_create();
  window_set_window_handlers(s_thread_window, (WindowHandlers) {
    .load = thread_window_load,
    .unload = thread_window_unload
  });

  s_detail_window = window_create();
  window_set_window_handlers(s_detail_window, (WindowHandlers) {
    .load = detail_window_load,
    .unload = detail_window_unload
  });

  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_open(1024, 1024);

  window_stack_push(s_host_window, true);
  update_stream_timer();
  schedule_refresh(600);
}

static void deinit(void) {
  if (s_refresh_timer) {
    app_timer_cancel(s_refresh_timer);
  }
  if (s_stream_timer) {
    app_timer_cancel(s_stream_timer);
  }
  if (s_dictation) {
    dictation_session_destroy(s_dictation);
  }
  if (s_host_window) {
    window_destroy(s_host_window);
  }
  if (s_thread_window) {
    window_destroy(s_thread_window);
  }
  if (s_detail_window) {
    window_destroy(s_detail_window);
  }
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
