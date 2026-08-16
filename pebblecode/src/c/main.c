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
#define CMD_THREAD_ACTION 15
#define CMD_PROJECT_NAME 16
#define CMD_PROJECT_PREVIEW 17
#define CMD_PROJECT_CREATE 18
#define CMD_CONCIERGE 19

#define KEY_SCOPE 24
#define KEY_OFFSET 25
#define KEY_MATCHED 26
#define KEY_OTHER 27
#define KEY_SETTLED 28
#define KEY_ACTION 29
#define KEY_PATH 30
#define KEY_NAME 31

/* Which slice of a host's threads the list is showing. Settled covers snoozed
   too: both mean the thread is not asking for anything right now. */
#define SCOPE_ACTIVE 0
#define SCOPE_SETTLED 1

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
/* Pebble Time 2 (emery): 200 x 228. Chrome takes 18 top and 18 bottom, the
   glass is inset 4 a side, so the panel is exactly 192 x 192 — which divides
   into four 48px rows with nothing left over. */
#if PBL_DISPLAY_WIDTH != 200 || PBL_DISPLAY_HEIGHT != 228
#error "Laid out for Pebble Time 2 (emery, 200x228)"
#endif
#define SCREEN_W PBL_DISPLAY_WIDTH
#define SCREEN_H PBL_DISPLAY_HEIGHT
#define LEGEND_HEIGHT 16
#define RAIL_HEIGHT 2
#define TOP_CHROME (LEGEND_HEIGHT + RAIL_HEIGHT)
#define BOTTOM_CHROME (LEGEND_HEIGHT + RAIL_HEIGHT)
#define PANEL_INSET 4
#define PANEL_W (SCREEN_W - 2 * PANEL_INSET)
#define PANEL_H (SCREEN_H - TOP_CHROME - BOTTOM_CHROME)
#define GLASS_PAD 8
#define ROW_HEIGHT 48
#define SECTION_HEADER_HEIGHT 18
#define CARD_PAD 8

#define STREAM_TICK_MS 110
#define GHOST_ROWS 4

typedef struct {
  char id[40];
  char title[40];
  char detail[40];
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
  char agent[32];
  char summary[180];
  bool settled;
} SessionItem;

typedef struct {
  char id[80];
  char title[52];
  char directory[40];
} ProjectItem;

typedef enum {
  DictationTargetSession,
  DictationTargetProject,
  DictationTargetNewProject,
  DictationTargetConcierge
} DictationTarget;

/* Rows that sit under the thread list and lead somewhere rather than opening a
   thread. Which ones exist depends on the scope and on how much is left to
   page through, so they are rebuilt whenever a listing lands. */
typedef enum {
  FooterScopeSettled,
  FooterScopeActive,
  FooterMore
} FooterKind;

/* What an action menu entry does. Stored as the item's action data. */
typedef enum {
  ActionReply = 1,
  ActionSettle,
  ActionUnsettle,
  ActionInterrupt,
  ActionCreateProject
} ActionKind;

/* Thread-list scope and paging state, all reported by the phone alongside a
   listing so the footer rows can be labelled without a second request. */
static int s_scope;
static int s_offset;
static int s_matched;
static int s_other;
static FooterKind s_footers[2];
static int s_footer_count;

/* A project path proposed by the phone, held while the user confirms it. */
static char s_pending_project_path[160];
static char s_pending_project_name[48];

static Window *s_host_window;
static Window *s_thread_window;
static Window *s_detail_window;
static Window *s_diag_window;
static Layer *s_diag_layer;
static MenuLayer *s_thread_menu;
static Layer *s_host_layer;
static Layer *s_thread_chrome;
static Layer *s_detail_header_layer;
static Layer *s_detail_layer;
static ScrollLayer *s_scroll_layer;
static AppTimer *s_refresh_timer;
static AppTimer *s_stream_timer;
static DictationSession *s_dictation;
static GFont s_font_dseg_big;
static GFont s_font_dseg_small;
static GFont s_font_tech;
static GFont s_font_tech_small;
static GBitmap *s_hatch;
static GBitmap *s_stipple;
static GColor s_hatch_palette[2];
static GColor s_stipple_palette[2];
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
static bool s_sending_message;
static bool s_link_down;
static time_t s_last_sync;
static bool s_detail_loading;

/* Errors are kept, not flashed. The reference prints MONITOR in red beneath
   the glass; that is where a fault belongs, and the log behind it is what
   makes a failure debuggable from the wrist. */
#define ERROR_LOG_DEPTH 6
static char s_error_log[ERROR_LOG_DEPTH][64];
static int s_error_log_count;
static int s_error_total;
static bool s_error_active;

static char s_status_text[40];
static char s_full_context[MAX_CONTEXT_TEXT];
static char s_pending_context_session_id[80];
static char s_pending_context_request_id[16];
static char s_draw_body[MAX_CONTEXT_TEXT];

static void log_error(const char *text);
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

static void log_error(const char *text) {
  if (!text || !text[0]) {
    return;
  }
  if (s_error_log_count > 0 && strncmp(s_error_log[0], text, sizeof(s_error_log[0]) - 1) == 0) {
    /* A repeating fault should not push the earlier context out of the log. */
    s_error_active = true;
    return;
  }
  for (int i = ERROR_LOG_DEPTH - 1; i > 0; i--) {
    strncpy(s_error_log[i], s_error_log[i - 1], sizeof(s_error_log[i]) - 1);
    s_error_log[i][sizeof(s_error_log[i]) - 1] = '\0';
  }
  strncpy(s_error_log[0], text, sizeof(s_error_log[0]) - 1);
  s_error_log[0][sizeof(s_error_log[0]) - 1] = '\0';
  if (s_error_log_count < ERROR_LOG_DEPTH) {
    s_error_log_count++;
  }
  s_error_total++;
  s_error_active = true;
}

/* ---------------------------------------------------------------- palette */

/* A negative-LCD Casio: near-black chassis, pale glass, dark ink, cyan
   legends, crimson rails. Monochrome watches collapse to black on white,
   which the shape language survives. */
/* A real positive LCD is a warm tan, not neutral grey; pale grey washes out
   on the physical panel. This is the substrate the reference photographs as. */
static GColor lcd_glass(void) {
#ifdef PBL_COLOR
  return GColorBrass;
#else
  return GColorWhite;
#endif
}

/* Unlit segments sit between the substrate and the ink, then get knocked back
   further by the hatch. */
static GColor lcd_ghost_tone(void) {
#ifdef PBL_COLOR
  return GColorPastelYellow;
#else
  return GColorBlack;
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

/* Red on the black chassis has to be bright to read; the same red on the pale
   glass has to be dark. One value cannot do both jobs. */
static GColor rule_color(void) {
#ifdef PBL_COLOR
  return GColorFolly;
#else
  return GColorWhite;
#endif
}

/* Alerts drawn on the glass: dark enough to hold against the substrate. */
static GColor alert_color(void) {
#ifdef PBL_COLOR
  return GColorDarkCandyAppleRed;
#else
  return GColorBlack;
#endif
}

/* Anything highlighted on the glass uses ink, never a chassis legend colour:
   light blue on a pale substrate is unreadable, and on the monochrome watches
   the legend colour is white, which disappears entirely. */
static GColor glass_accent(void) {
  return GColorBlack;
}

static GColor active_color(void) {
#ifdef PBL_COLOR
  return GColorDukeBlue;
#else
  return GColorBlack;
#endif
}

/* Share Tech Mono carries every label and title: a fixed-width technical face
   is what makes the whole app read as an instrument rather than a phone list.
   Long prose stays on the system font, which is far easier to read at length. */
static GFont font_row_title(void) {
  return s_font_tech ? s_font_tech : fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
}

static GFont font_row_detail(void) {
  return s_font_tech_small ? s_font_tech_small : fonts_get_system_font(FONT_KEY_GOTHIC_14);
}

static GFont font_legend(void) {
  return s_font_tech_small ? s_font_tech_small : fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
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
         s_detail_loading || !s_hosts_synced || any_live_row();
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
  if (s_diag_layer && window_stack_get_top_window() == s_diag_window) {
    layer_mark_dirty(s_diag_layer);
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

/* Pebble draws text from the top of the layout box, which sits above the cap
   height by the font's internal leading — so hand-tuned negative offsets were
   guesses that only held for one font at one size. These measure the glyph box
   and place it exactly, which is what makes the result repeatable. */
typedef struct {
  int top;   /* rows between the layout box top and the first inked row */
  int height; /* inked rows for upper-case text */
} InkBox;

/* Measured from watch screenshots, not guessed: Share Tech Mono at 16 inks
   rows 7..14 of its layout box, and at 20 rows 9..18. */
static const InkBox INK_TECH_16 = { 7, 8 };
static const InkBox INK_TECH_20 = { 9, 10 };

static InkBox ink_for(GFont font) {
  return font == s_font_tech ? INK_TECH_20 : INK_TECH_16;
}

/* Places text so its inked rows are centred in `box`. */
static int ink_origin_y(GRect box, GFont font) {
  InkBox ink = ink_for(font);
  return box.origin.y + (box.size.h - ink.height) / 2 - ink.top;
}

static GSize measure(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, SCREEN_W, SCREEN_H),
                                               GTextOverflowModeTrailingEllipsis,
                                               GTextAlignmentLeft);
}

/* Draws `text` vertically centred inside `box`, left-aligned at box.x. */
static void draw_text_v(GContext *ctx, const char *text, GFont font, GRect box,
                        GTextAlignment align) {
  GSize size = measure(text, font);
  int y = box.origin.y + (box.size.h - size.h) / 2;
  graphics_draw_text(ctx, text, font, GRect(box.origin.x, y, box.size.w, size.h + 2),
                     GTextOverflowModeTrailingEllipsis, align, NULL);
}


/* Two 1-bit tiles do all the LCD texture work, blitted rather than drawn
   pixel by pixel: a stipple that gives the substrate its panel grain, and a
   hatch that knocks unlit segments back so they read as texture instead of
   competing with the lit ones. Both are built once at launch. */
#define STIPPLE_SPACING 4

static void lcd_tiles_create(void) {
  s_hatch_palette[0] = GColorClear;
  s_hatch_palette[1] = lcd_glass();
  s_hatch = gbitmap_create_blank_with_palette(GSize(8, 8), GBitmapFormat1BitPalette,
                                              s_hatch_palette, false);
  if (s_hatch) {
    uint8_t *data = gbitmap_get_data(s_hatch);
    uint16_t stride = gbitmap_get_bytes_per_row(s_hatch);
    for (int y = 0; y < 8; y++) {
      /* 75% coverage: one ghost pixel in four survives. */
      data[y * stride] = (y % 2 == 0) ? 0xAA : 0xFF;
    }
  }

  s_stipple_palette[0] = GColorClear;
  s_stipple_palette[1] = lcd_dim();
  s_stipple = gbitmap_create_blank_with_palette(GSize(8, 8), GBitmapFormat1BitPalette,
                                                s_stipple_palette, false);
  if (s_stipple) {
    uint8_t *data = gbitmap_get_data(s_stipple);
    uint16_t stride = gbitmap_get_bytes_per_row(s_stipple);
    for (int y = 0; y < 8; y += STIPPLE_SPACING) {
      for (int x = 0; x < 8; x += STIPPLE_SPACING) {
        data[y * stride + x / 8] |= 0x80 >> (x % 8);
      }
    }
  }
}

static void lcd_tiles_destroy(void) {
  if (s_hatch) {
    gbitmap_destroy(s_hatch);
    s_hatch = NULL;
  }
  if (s_stipple) {
    gbitmap_destroy(s_stipple);
    s_stipple = NULL;
  }
}

static void hatch_rect(GContext *ctx, GRect r) {
  if (!s_hatch) {
    return;
  }
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, s_hatch, r);
}

static void stipple_rect(GContext *ctx, GRect r) {
#ifndef PBL_COLOR
  /* On a 1-bit panel a dot grid is noise, not texture: the substrate is
     already the brightest thing available. */
  (void)r;
  return;
#endif
  if (!s_stipple) {
    return;
  }
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, s_stipple, r);
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

/* Labels are drawn as whole strings.

   These used to render glyph by glyph to fake letter-spacing, stepping by each
   character's measured width. That was the cause of the uneven, clipped text:
   every glyph was re-positioned and re-clipped into its own box, so stems were
   cut and the pitch wandered. Share Tech Mono is already a fixed-pitch
   technical face — it does not need faked tracking, and one draw call per
   string is both correct and cheaper. The `tracking` argument is retained so
   call sites did not all have to change; it is deliberately ignored. */
static int tracked_width(const char *text, GFont font, int tracking) {
  (void)tracking;
  return measure(text, font).w;
}

static void draw_tracked_max(GContext *ctx, const char *text, GFont font, GPoint origin,
                             int tracking, int max_w) {
  (void)tracking;
  GSize size = measure(text, font);
  int w = max_w > 0 ? max_w : size.w + 2;
  graphics_draw_text(ctx, text, font, GRect(origin.x, origin.y, w, size.h + 2),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void draw_tracked(GContext *ctx, const char *text, GFont font, GPoint origin, int tracking) {
  draw_tracked_max(ctx, text, font, origin, tracking, 0);
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

/* Segment digits come from DSEG7, drawn the way a real panel works: the full
   "88" is laid down in the ghost tone first, the hatch knocks it back, then
   the live value goes on top. Nothing shifts when a digit is blank, because
   DSEG is fixed-width and the unlit segments stay put. */
static GSize seg_text_size(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, 160, 80),
                                               GTextOverflowModeTrailingEllipsis,
                                               GTextAlignmentLeft);
}

static void draw_seg_value(GContext *ctx, GPoint at, int value, int digits, GColor on, bool big) {
  GFont font = big ? s_font_dseg_big : s_font_dseg_small;
  if (!font) {
    /* A resource that failed to load must not leave a hole where the reading
       goes: fall back to the label face rather than drawing nothing. */
    char fallback[8];
    snprintf(fallback, sizeof(fallback), "%d", value);
    graphics_context_set_text_color(ctx, on);
    graphics_draw_text(ctx, fallback, big ? font_row_title() : font_row_detail(),
                       GRect(at.x, at.y, 44, big ? 30 : 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }
  char ghost[4] = "88";
  char live[8];
  if (digits >= 3) {
    strncpy(ghost, "888", sizeof(ghost) - 1);
  } else if (digits == 1) {
    strncpy(ghost, "8", sizeof(ghost) - 1);
  }
  snprintf(live, sizeof(live), "%d", value);

  GSize full = seg_text_size(ghost, font);
  GSize live_size = seg_text_size(live, font);

  graphics_context_set_text_color(ctx, lcd_ghost_tone());
  graphics_draw_text(ctx, ghost, font, GRect(at.x, at.y, full.w + 4, full.h + 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  hatch_rect(ctx, GRect(at.x, at.y, full.w + 2, full.h + 2));

  /* Right-aligned into the ghost so the ones column never moves. */
  graphics_context_set_text_color(ctx, on);
  graphics_draw_text(ctx, live, font,
                     GRect(at.x + full.w - live_size.w, at.y, live_size.w + 4, full.h + 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static GSize seg_block_size(int digits, bool big) {
  GFont font = big ? s_font_dseg_big : s_font_dseg_small;
  if (!font) {
    return GSize(big ? 44 : 22, big ? 30 : 18);
  }
  const char *ghost = digits >= 3 ? "888" : (digits == 1 ? "8" : "88");
  return seg_text_size(ghost, font);
}

/* A BAT-style segmented meter. The reference spends this on battery; here it
   carries how much of the list is behind you, so the band stays useful. */
static void draw_segment_meter(GContext *ctx, GPoint origin, int height, int filled, int total, GColor on) {
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
    graphics_context_set_fill_color(ctx, i < lit ? on : lcd_dim());
    graphics_fill_rect(ctx, GRect(origin.x + i * (cell_w + gap), origin.y, cell_w, height), 0, GCornerNone);
  }
}

/* The day strip, reused as a host strip: one square per configured machine,
   the one in view filled in. */
static void draw_host_strip(GContext *ctx, GPoint origin, int count, int current, GColor on) {
  int cell = 5;
  int gap = 3;
  for (int i = 0; i < count && i < MAX_HOSTS; i++) {
    GRect mark = GRect(origin.x + i * (cell + gap), origin.y, cell, cell);
    graphics_context_set_stroke_color(ctx, on);
    graphics_context_set_fill_color(ctx, on);
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
  stipple_rect(ctx, grect_inset(panel, GEdgeInsets(2)));
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
  /* Right-hand fields are laid out first and the left label is bounded by what
     remains, so a long name can never run under the counters. */
  int right_edge = bounds.size.w - GLASS_PAD;
  int band_y = ink_origin_y(GRect(0, 0, 0, LEGEND_HEIGHT), font_legend());
  char tail[24];

  if (s_error_total > 0) {
    snprintf(tail, sizeof(tail), "ERR%d", s_error_total);
    int ew = tracked_width(tail, font_legend(), 1);
    graphics_context_set_text_color(ctx, rule_color());
    draw_tracked(ctx, tail, font_legend(), GPoint(right_edge - ew, band_y), 1);
    right_edge -= ew + 6;
  }
  if (right && right[0]) {
    int w = tracked_width(right, font_legend(), 1);
    graphics_context_set_text_color(ctx, legend());
    draw_tracked(ctx, right, font_legend(), GPoint(right_edge - w, band_y), 1);
    right_edge -= w + 6;
  }
  if (left && left[0]) {
    graphics_context_set_text_color(ctx, legend());
    draw_tracked_max(ctx, left, font_legend(), GPoint(GLASS_PAD, band_y), 1, right_edge - GLASS_PAD);
  }
}

/* The bottom band: normally a legend, but a live fault takes it over in
   crimson the way MONITOR does on the reference. */
static void draw_bottom_band(GContext *ctx, GRect bounds, const char *left, const char *right) {
  GRect band = GRect(GLASS_PAD, bounds.size.h - BOTTOM_CHROME + RAIL_HEIGHT,
                     bounds.size.w - 2 * GLASS_PAD, LEGEND_HEIGHT - RAIL_HEIGHT);
  int y = ink_origin_y(band, font_legend());
  if (s_error_active && s_error_log_count > 0) {
    graphics_context_set_text_color(ctx, rule_color());
    draw_tracked(ctx, "ERR", font_legend(), GPoint(band.origin.x, y), 1);
    int lead = tracked_width("ERR", font_legend(), 1) + 6;
    graphics_context_set_text_color(ctx, legend());
    draw_text_v(ctx, s_error_log[0], font_row_detail(),
                GRect(band.origin.x + lead, band.origin.y, band.size.w - lead, band.size.h),
                GTextAlignmentLeft);
    return;
  }
  graphics_context_set_text_color(ctx, legend());
  if (left && left[0]) {
    draw_tracked(ctx, left, font_legend(), GPoint(band.origin.x, y), 1);
  }
  if (right && right[0]) {
    int w = tracked_width(right, font_legend(), 1);
    draw_tracked(ctx, right, font_legend(), GPoint(band.origin.x + band.size.w - w, y), 1);
  }
}

/* The rail turns into the progress indicator while the phone is working, so
   nothing extra has to appear on screen. */
static void draw_busy_rail(GContext *ctx, GRect bounds, int y) {
  graphics_context_set_fill_color(ctx, chassis());
  graphics_fill_rect(ctx, GRect(0, y - 4, bounds.size.w, RAIL_HEIGHT + 4), 0, GCornerNone);
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
  GFont font = s_font_dseg_big;
  int y = panel.origin.y + 10;
  if (font) {
    GSize full = seg_text_size("88:88", font);
    int x = panel.origin.x + (panel.size.w - full.w) / 2;
    graphics_context_set_text_color(ctx, lcd_ghost_tone());
    graphics_draw_text(ctx, "88:88", font, GRect(x, y, full.w + 4, full.h + 4),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    hatch_rect(ctx, GRect(x, y, full.w + 2, full.h + 2));

    /* One digit lights at a time, walking the field like a power-on test. */
    static const char *steps[5] = { "8", " 8", "   8", "    8", "" };
    int lit = (s_stream_phase / 6) % 6;
    if (lit < 5 && steps[lit][0]) {
      graphics_context_set_text_color(ctx, lcd_ink());
      graphics_draw_text(ctx, steps[lit], font, GRect(x, y, full.w + 4, full.h + 4),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    }
    y += full.h + 4;
  } else {
    y += 34;
  }

  graphics_context_set_fill_color(ctx, lcd_dim());
  graphics_fill_rect(ctx, GRect(panel.origin.x + 8, y, panel.size.w - 16, 1), 0, GCornerNone);

  graphics_context_set_text_color(ctx, lcd_ink());
  int tw = tracked_width(title, font_legend(), 2);
  draw_tracked(ctx, title, font_legend(), GPoint(panel.origin.x + (panel.size.w - tw) / 2, y + 3), 2);

  graphics_context_set_text_color(ctx, lcd_ink());
  graphics_draw_text(ctx, hint, font_row_detail(),
                     GRect(panel.origin.x + 6, y + 20, panel.size.w - 12, 34),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  char age[12];
  sync_age_text(age, sizeof(age));
  char foot[28];
  snprintf(foot, sizeof(foot), "%s  SYN %s", BUILD_LABEL, age);
  int fw = tracked_width(foot, font_row_detail(), 1);
  draw_caption(ctx, foot, GPoint(panel.origin.x + (panel.size.w - fw) / 2,
                                 panel.origin.y + panel.size.h - 17), lcd_ink());
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
    draw_bottom_band(ctx, bounds, "DIAG HOLD", "RETRY");
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
                     GRect(inner_x, panel.origin.y + 2, inner_w, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  char sub[28];
  snprintf(sub, sizeof(sub), "%d THREAD%s", total, total == 1 ? "" : "S");
  draw_caption(ctx, sub, GPoint(inner_x, panel.origin.y + 19), lcd_ink());

  /* Field two: the boxed complication, on its own full-width row. It was laid
     out on a guessed pitch before, which overlapped the digits and spilled them
     out of the box; the pitch is now measured from the font, which is the only
     thing that can be right across three screen sizes. */
  GSize seg2 = seg_block_size(2, false);
  int cap_w = 11;
  int cell_w = cap_w + seg2.w;
  int slack = inner_w - 3 * cell_w;
  int gap = slack > 0 ? slack / 4 : 2;
  GRect field = GRect(inner_x, panel.origin.y + 33, inner_w, seg2.h + 6);
  draw_field_box(ctx, field);

  int trio[3] = { host->run, host->idle, host->settled };
  const char *trio_caps[3] = { "R", "I", "S" };
  for (int i = 0; i < 3; i++) {
    int cell_x = field.origin.x + gap + i * (cell_w + gap);
    draw_caption(ctx, trio_caps[i], GPoint(cell_x, field.origin.y + 2), lcd_ink());
    draw_seg_value(ctx, GPoint(cell_x + cap_w, field.origin.y + 3),
                   clamp_int(trio[i], 0, 99), 2, lcd_ink(), false);
  }

  /* Field three: the headline. One big segment readout, the way the time
     dominates the reference, with two caption lines beside it. */
  int big_y = field.origin.y + field.size.h + 6;
  GColor ink = state_color(host->state);
  if (gcolor_equal(ink, lcd_dim())) {
    ink = lcd_ink();
  }
  draw_seg_value(ctx, GPoint(inner_x, big_y), clamp_int(state_headline_count(host), 0, 99),
                 2, ink, true);

  GSize big_block = seg_block_size(2, true);
  int label_x = inner_x + big_block.w + 10;
  graphics_context_set_text_color(ctx, lcd_ink());
  draw_tracked(ctx, state_word(host->state), font_legend(), GPoint(label_x, big_y + 2), 1);
  if (state_is(host->state, "offline")) {
    graphics_draw_text(ctx, host->detail[0] ? host->detail : "no answer",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(label_x, big_y + 14, panel.size.w - (label_x - panel.origin.x) - 8, 30),
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  } else {
    char of_line[24];
    snprintf(of_line, sizeof(of_line), "OF %d", total);
    draw_caption(ctx, of_line, GPoint(label_x, big_y + 17), lcd_ink());
  }
  draw_caption(ctx, state_is(host->state, "offline") ? "TAP RETRY" : "TAP OPEN",
               GPoint(label_x, big_y + 30), lcd_ink());

  if (state_is_live(host->state) || state_is(host->state, "needs")) {
    draw_state_mark(ctx, GRect(panel.origin.x + panel.size.w - 15, big_y + 4, 7, 7), host->state, false);
  }

  /* The indicator row: hairline, meter with its caption, sync age, and the
     day strip carrying which machine is in view. */
  int strip_y = panel.origin.y + panel.size.h - 15;
  graphics_context_set_fill_color(ctx, lcd_dim());
  graphics_fill_rect(ctx, GRect(inner_x, strip_y - 5, inner_w, 1), 0, GCornerNone);

  draw_caption(ctx, "ACT", GPoint(inner_x, strip_y - 4), lcd_ink());
  draw_segment_meter(ctx, GPoint(inner_x + 24, strip_y + 2), 7, total - host->settled, total, glass_accent());

  char age[12];
  sync_age_text(age, sizeof(age));
  char sync_line[20];
  snprintf(sync_line, sizeof(sync_line), "SYN %s", age);
  draw_caption(ctx, sync_line, GPoint(inner_x + 62, strip_y - 4), lcd_ink());

  draw_host_strip(ctx, GPoint(panel.origin.x + panel.size.w - 8 - s_host_count * 8, strip_y + 1),
                  s_host_count, s_host_cursor, glass_accent());

  draw_bottom_band(ctx, bounds, "HOST", "OPEN");
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

static void host_select_long(ClickRecognizerRef recognizer, void *context) {
  if (s_diag_window) {
    window_stack_push(s_diag_window, true);
  }
}

static void host_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, host_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, host_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, host_select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, host_select_long, NULL);
}

/* Asks the phone for one page of one scope. The list is cleared first so a
   stale page is never left on the glass while the next one is in flight. */
static void request_host_threads(int scope, int offset) {
  s_scope = scope;
  s_offset = offset < 0 ? 0 : offset;
  s_session_count = 0;
  s_project_count = 0;
  s_footer_count = 0;
  s_threads_synced = false;
  s_loading_threads = true;
  update_stream_timer();
  mark_all_dirty();

  DictionaryIterator *iter = NULL;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK || !iter) {
    log_error("Phone link busy");
    set_status("Phone link busy");
    return;
  }
  int command = CMD_SELECT_HOST;
  dict_write_int(iter, KEY_CMD, &command, sizeof(command), true);
  dict_write_cstring(iter, KEY_HOST_ID, s_hosts[s_selected_host_index].id);
  dict_write_int(iter, KEY_SCOPE, &s_scope, sizeof(s_scope), true);
  dict_write_int(iter, KEY_OFFSET, &s_offset, sizeof(s_offset), true);
  app_message_outbox_send();
}

static void open_selected_host(void) {
  s_selected_host_index = clamp_int(s_host_cursor, 0, s_host_count - 1);
  window_stack_push(s_thread_window, true);
  /* A host always opens on the work that still wants something. */
  request_host_threads(SCOPE_ACTIVE, 0);
}

/* ------------------------------------------------------------ diagnostics */

/* Held behind the host screen: what the app knows about itself, and the fault
   log in full. Rendered on the same glass as everything else so debugging
   never drops out of the theme. */
static void diag_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GRect panel = draw_panel(ctx, bounds);

  char count[16];
  snprintf(count, sizeof(count), "%d", s_error_total);
  draw_legend_band(ctx, bounds, "DIAG", s_error_total > 0 ? "" : count);
  draw_rail(ctx, bounds, LEGEND_HEIGHT, true);
  draw_rail(ctx, bounds, bounds.size.h - BOTTOM_CHROME, false);

  int x = panel.origin.x + 7;
  int w = panel.size.w - 14;
  int y = panel.origin.y + 2;

  char age[12];
  sync_age_text(age, sizeof(age));
  char line[64];

  snprintf(line, sizeof(line), "%s   LINK %s", BUILD_LABEL, s_link_down ? "DOWN" : "OK");
  graphics_context_set_text_color(ctx, lcd_ink());
  draw_tracked(ctx, line, font_legend(), GPoint(x, y), 1);
  y += 15;

  snprintf(line, sizeof(line), "HOSTS %d   THR %d   SYN %s", s_host_count, s_session_count, age);
  draw_caption(ctx, line, GPoint(x, y), lcd_ink());
  y += 14;

  graphics_context_set_fill_color(ctx, lcd_dim());
  graphics_fill_rect(ctx, GRect(x, y, w, 1), 0, GCornerNone);
  y += 4;

  if (s_error_log_count == 0) {
    draw_caption(ctx, "NO FAULTS LOGGED", GPoint(x, y + 4), lcd_ink());
  } else {
    for (int i = 0; i < s_error_log_count && y < panel.origin.y + panel.size.h - 16; i++) {
      char idx[6];
      snprintf(idx, sizeof(idx), "%d", s_error_total - i);
      graphics_context_set_text_color(ctx, alert_color());
      draw_tracked(ctx, idx, font_legend(), GPoint(x, y - 2), 1);
      graphics_context_set_text_color(ctx, lcd_ink());
      graphics_draw_text(ctx, s_error_log[i], fonts_get_system_font(FONT_KEY_GOTHIC_14),
                         GRect(x + 16, y - 3, w - 16, 30),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
      y += 16;
    }
  }

  draw_bottom_band(ctx, bounds, "FAULTS", "BACK");
}

static void diag_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  window_set_background_color(window, chassis());
  s_diag_layer = layer_create(layer_get_bounds(window_layer));
  layer_set_update_proc(s_diag_layer, diag_layer_update_proc);
  layer_add_child(window_layer, s_diag_layer);
}

static void diag_window_unload(Window *window) {
  if (s_diag_layer) {
    layer_destroy(s_diag_layer);
  }
  s_diag_layer = NULL;
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

  graphics_context_set_text_color(ctx, selected ? lcd_glass() : lcd_ink());
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
  draw_bottom_band(ctx, bounds, tally, "");
  if (!s_error_active) {
    draw_host_strip(ctx, GPoint(bounds.size.w - 8 - s_host_count * 8, bounds.size.h - BOTTOM_CHROME + 5),
                    s_host_count, s_selected_host_index, legend());
  }
}

/* Footer rows are derived, never stored by the phone: which ones apply follows
   from the scope and from whether this page is the last one. */
static void rebuild_footers(void) {
  s_footer_count = 0;
  if (!s_threads_synced || s_loading_threads) {
    return;
  }
  if (s_scope == SCOPE_ACTIVE) {
    if (s_other > 0) {
      s_footers[s_footer_count++] = FooterScopeSettled;
    }
    return;
  }
  if (s_offset + s_session_count < s_matched) {
    s_footers[s_footer_count++] = FooterMore;
  }
  s_footers[s_footer_count++] = FooterScopeActive;
}

/* Rows in section 0 that are threads rather than footers. The empty list still
   occupies one row, so the footers always sit below something. */
static int thread_body_rows(void) {
  if (!s_threads_synced || s_loading_threads) {
    return GHOST_ROWS;
  }
  return s_session_count > 0 ? s_session_count : 1;
}

static void footer_label(FooterKind kind, char *out, size_t out_size) {
  switch (kind) {
    case FooterScopeSettled:
      snprintf(out, out_size, "SETTLED %d", s_other);
      return;
    case FooterScopeActive:
      snprintf(out, out_size, "ACTIVE %d", s_other);
      return;
    case FooterMore:
      snprintf(out, out_size, "MORE %d OF %d",
               s_offset + s_session_count, s_matched);
      return;
  }
  out[0] = '\0';
}

static uint16_t thread_num_sections(MenuLayer *menu_layer, void *data) {
  return 2;
}

static uint16_t thread_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  if (section_index == 1) {
    /* One row past the projects is the way to make a new one. */
    return (!s_threads_synced || s_loading_threads) ? 0 : s_project_count + 1;
  }
  return thread_body_rows() + s_footer_count;
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
  draw_tracked(ctx, "START NEW", font_legend(),
               GPoint(GLASS_PAD, ink_origin_y(bounds, font_legend())), 1);
}

static bool thread_list_is_empty(void) {
  return s_threads_synced && !s_loading_threads && s_session_count == 0 && s_project_count == 0;
}

static int16_t thread_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  if (thread_list_is_empty() && cell_index->section == 0) {
    /* One full-height cell so the glass shows a panel rather than a stray row
       floating in empty space. */
    return s_thread_menu ? layer_get_bounds(menu_layer_get_layer(s_thread_menu)).size.h : ROW_HEIGHT;
  }
  return ROW_HEIGHT;
}

static void thread_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
  bool selected = menu_layer_is_index_selected(s_thread_menu, cell_index);

  if (cell_index->section == 1) {
    if (cell_index->row == s_project_count) {
      draw_list_row(ctx, cell_layer, selected, "New project", "dictate a name", "empty");
      return;
    }
    if (cell_index->row > s_project_count) {
      return;
    }
    ProjectItem *project = &s_projects[cell_index->row];
    draw_list_row(ctx, cell_layer, selected, project->title, project->directory, "idle");
    return;
  }

  /* Footer rows sit past the threads and lead elsewhere rather than opening
     one, so they are drawn before the thread cases below. */
  int body = thread_body_rows();
  if (s_threads_synced && !s_loading_threads && cell_index->row >= body) {
    int footer = cell_index->row - body;
    if (footer < s_footer_count) {
      char label[24];
      footer_label(s_footers[footer], label, sizeof(label));
      draw_list_row(ctx, cell_layer, selected, label,
                    s_footers[footer] == FooterMore ? "next page" : "switch list", "empty");
    }
    return;
  }

  if (!s_threads_synced || s_loading_threads) {
    draw_ghost_row(ctx, layer_get_bounds(cell_layer), cell_index->row);
    return;
  }
  if (s_session_count == 0) {
    GRect bounds = layer_get_bounds(cell_layer);
    graphics_context_set_fill_color(ctx, lcd_glass());
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    if (thread_list_is_empty()) {
      draw_self_test(ctx, bounds, "NO THREADS", "Nothing open on this machine");
    } else {
      draw_list_row(ctx, cell_layer, selected, "No threads", "start one below", "empty");
    }
    return;
  }
  if (cell_index->row >= s_session_count) {
    return;
  }
  SessionItem *item = &s_sessions[cell_index->row];
  draw_list_row(ctx, cell_layer, selected, item->title, item->detail, item->state);
}

/* Holding the new-project row describes a location instead of naming one: the
   concierge agent works out the path and it comes back to the same confirm. */
static void thread_select_long(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  if (cell_index->section != 1 || cell_index->row != s_project_count) {
    return;
  }
  s_dictation_target = DictationTargetConcierge;
  set_status("Describe where");
  start_dictation();
}

static void thread_select(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  if (cell_index->section == 1) {
    if (cell_index->row == s_project_count) {
      s_dictation_target = DictationTargetNewProject;
      set_status("Name the project");
      start_dictation();
      return;
    }
    if (cell_index->row > s_project_count) {
      return;
    }
    s_selected_project_index = cell_index->row;
    s_dictation_target = DictationTargetProject;
    set_status("Dictate prompt");
    start_dictation();
    return;
  }

  int body = thread_body_rows();
  if (s_threads_synced && !s_loading_threads && cell_index->row >= body) {
    int footer = cell_index->row - body;
    if (footer >= s_footer_count) {
      return;
    }
    switch (s_footers[footer]) {
      case FooterScopeSettled:
        request_host_threads(SCOPE_SETTLED, 0);
        return;
      case FooterScopeActive:
        request_host_threads(SCOPE_ACTIVE, 0);
        return;
      case FooterMore:
        request_host_threads(s_scope, s_offset + s_session_count);
        return;
    }
    return;
  }

  if (!s_threads_synced || s_session_count == 0 || cell_index->row >= s_session_count) {
    return;
  }
  s_selected_index = cell_index->row;
  s_showing_context = false;
  s_context_loading = false;
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
    log_error("Phone link busy");
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
  s_detail_loading = true;
  s_pending_context_session_id[0] = '\0';
  s_pending_context_request_id[0] = '\0';
  s_context_page = 0;
  s_context_page_count = 1;
  s_full_context[0] = '\0';
  update_detail_text();
  update_stream_timer();

  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_DETAIL);
  if (!iter) {
    return;
  }
  dict_write_int(iter, KEY_INDEX, &index, sizeof(index), true);
  dict_write_cstring(iter, KEY_SESSION_ID, s_sessions[index].id);
  app_message_outbox_send();
}

/* Settle, unsettle and interrupt all name the selected thread and differ only
   in the verb, so they share one message. */
static void send_thread_action(const char *action) {
  if (s_selected_index < 0 || s_selected_index >= s_session_count) {
    return;
  }
  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_THREAD_ACTION);
  if (!iter) {
    return;
  }
  dict_write_cstring(iter, KEY_SESSION_ID, s_sessions[s_selected_index].id);
  dict_write_cstring(iter, KEY_ACTION, action);
  app_message_outbox_send();
}

static void send_project_create(void) {
  if (!s_pending_project_path[0]) {
    return;
  }
  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_PROJECT_CREATE);
  if (!iter) {
    return;
  }
  dict_write_cstring(iter, KEY_HOST_ID, s_hosts[s_selected_host_index].id);
  dict_write_cstring(iter, KEY_NAME, s_pending_project_name);
  dict_write_cstring(iter, KEY_PATH, s_pending_project_path);
  app_message_outbox_send();
  s_pending_project_path[0] = '\0';
  s_pending_project_name[0] = '\0';
}

/* A dictated name is only a proposal: the phone turns it into an absolute path
   and sends it back for confirmation before anything is created. */
static void send_project_name(const char *name) {
  if (s_selected_host_index < 0 || s_selected_host_index >= s_host_count) {
    return;
  }
  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_PROJECT_NAME);
  if (!iter) {
    return;
  }
  dict_write_cstring(iter, KEY_HOST_ID, s_hosts[s_selected_host_index].id);
  dict_write_cstring(iter, KEY_NAME, name);
  app_message_outbox_send();
}

/* For when the location is easier described than dictated. An agent in the
   configured project proposes the path; confirmation still happens here. */
static void send_concierge_text(const char *text) {
  if (s_selected_host_index < 0 || s_selected_host_index >= s_host_count) {
    return;
  }
  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_CONCIERGE);
  if (!iter) {
    return;
  }
  dict_write_cstring(iter, KEY_HOST_ID, s_hosts[s_selected_host_index].id);
  dict_write_cstring(iter, KEY_PROMPT, text);
  app_message_outbox_send();
}

/* The second parameter is the performed item, not the root level, whatever the
   SDK's doc comment says, so the level to free is carried in the context. */
static void action_menu_closed(ActionMenu *menu, const ActionMenuItem *performed, void *context) {
  action_menu_hierarchy_destroy((const ActionMenuLevel *)context, NULL, NULL);
}

static void perform_action(ActionMenu *menu, const ActionMenuItem *action, void *context) {
  switch ((ActionKind)(uintptr_t)action_menu_item_get_action_data(action)) {
    case ActionReply:
      s_dictation_target = DictationTargetSession;
      start_dictation();
      return;
    case ActionSettle:
      set_status("Settling");
      send_thread_action("settle");
      return;
    case ActionUnsettle:
      set_status("Reopening");
      send_thread_action("unsettle");
      return;
    case ActionInterrupt:
      set_status("Interrupting");
      send_thread_action("interrupt");
      return;
    case ActionCreateProject:
      set_status("Creating project");
      send_project_create();
      return;
  }
}

static void open_action_menu(ActionMenuLevel *level) {
  ActionMenuConfig config = (ActionMenuConfig) {
    .root_level = level,
    .context = level,
    .colors = { .background = legend(), .foreground = lcd_ink() },
    .did_close = action_menu_closed,
    .align = ActionMenuAlignCenter
  };
  action_menu_open(&config);
}

/* Built per thread so it only ever offers what applies: a settled thread can
   be reopened but not settled again, and vice versa. */
static void open_thread_actions(void) {
  if (s_selected_index < 0 || s_selected_index >= s_session_count) {
    return;
  }
  ActionMenuLevel *level = action_menu_level_create(3);
  if (!level) {
    log_error("Out of memory");
    return;
  }
  action_menu_level_add_action(level, "Reply", perform_action, (void *)(uintptr_t)ActionReply);
  if (s_sessions[s_selected_index].settled) {
    action_menu_level_add_action(level, "Unsettle", perform_action, (void *)(uintptr_t)ActionUnsettle);
  } else {
    action_menu_level_add_action(level, "Settle", perform_action, (void *)(uintptr_t)ActionSettle);
    action_menu_level_add_action(level, "Interrupt", perform_action, (void *)(uintptr_t)ActionInterrupt);
  }
  open_action_menu(level);
}

/* The confirm gate for a dictated project: the phone has resolved a path, and
   nothing is created until this menu is answered. */
static void open_project_confirm(void) {
  ActionMenuLevel *level = action_menu_level_create(1);
  if (!level) {
    log_error("Out of memory");
    return;
  }
  action_menu_level_add_action(level, "Create", perform_action, (void *)(uintptr_t)ActionCreateProject);
  open_action_menu(level);
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
  int height = 42 + text_height(summary, font_body(), width - 20) + 48;
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

  graphics_context_set_text_color(ctx, lcd_ink());
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
  if (s_detail_loading) {
    graphics_context_set_text_color(ctx, lcd_ink());
    graphics_draw_text(ctx, item->title, font_row_title(),
                       GRect(10, -2, bounds.size.w - 20, 21),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_context_set_fill_color(ctx, lcd_dim());
    graphics_fill_rect(ctx, GRect(10, 34, bounds.size.w - 20, 1), 0, GCornerNone);
    for (int i = 0; i < 4; i++) {
      int lit = (s_stream_phase / 3) % 5;
      graphics_context_set_fill_color(ctx, i == lit ? lcd_ink() : lcd_dim());
      int w = bounds.size.w - 26 - (i == 3 ? 40 : 0);
      graphics_fill_rect(ctx, GRect(13, 42 + i * 14, w, 8), 0, GCornerNone);
    }
    return;
  }
  const char *summary = item->summary[0] ? item->summary : "No summary yet.";
  graphics_context_set_text_color(ctx, lcd_ink());
  graphics_draw_text(ctx, item->title, font_row_title(),
                     GRect(10, -2, bounds.size.w - 20, 21),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  draw_caption(ctx, item->detail, GPoint(10, 17), lcd_ink());
  if (item->agent[0]) {
    int aw = tracked_width(item->agent, font_row_detail(), 1);
    draw_caption(ctx, item->agent, GPoint(bounds.size.w - 10 - aw, 17), lcd_ink());
  }
  graphics_context_set_fill_color(ctx, lcd_dim());
  graphics_fill_rect(ctx, GRect(10, 34, bounds.size.w - 20, 1), 0, GCornerNone);
  graphics_context_set_text_color(ctx, lcd_ink());
  graphics_draw_text(ctx, summary, font_body(), GRect(10, 38, bounds.size.w - 20, bounds.size.h - 62),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  draw_caption(ctx, "SELECT LOG   HOLD SPEAK", GPoint(10, bounds.size.h - 20), lcd_ink());
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
  draw_legend_band(ctx, bounds, s_showing_context ? "LOG" : "THREAD", right);
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
  s_context_page = 0;
  s_context_page_count = 1;
  s_full_context[0] = '\0';
  detail_reset_scroll(false);
  update_detail_text();
  update_stream_timer();
}

static void detail_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_showing_context) {
    /* Already reading: jump to the newest page, which is the one worth
       reaching quickly in a long log. */
    if (s_context_page != s_context_page_count - 1) {
      request_context_page(s_context_page_count - 1);
    }
    return;
  }
  request_detail(s_selected_index, true);
}

/* Reply stays on a short press because it is the common case; everything else
   lives behind this menu rather than competing for the four buttons. */
static void detail_select_long_handler(ClickRecognizerRef recognizer, void *context) {
  open_thread_actions();
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

static bool detail_at_top(void) {
  if (!s_scroll_layer) {
    return true;
  }
  return scroll_layer_get_content_offset(s_scroll_layer).y >= 0;
}

static bool detail_at_bottom(void) {
  if (!s_scroll_layer) {
    return true;
  }
  return -scroll_layer_get_content_offset(s_scroll_layer).y >= detail_scroll_max_offset();
}

static void detail_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_showing_context && detail_at_top() && s_context_page > 0) {
    request_context_page(s_context_page - 1);
    return;
  }
  detail_scroll_by(-40);
}

static void detail_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_showing_context && detail_at_bottom() && s_context_page < s_context_page_count - 1) {
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
    } else if (s_dictation_target == DictationTargetNewProject) {
      send_project_name(transcription);
    } else if (s_dictation_target == DictationTargetConcierge) {
      send_concierge_text(transcription);
    } else {
      send_prompt_text(transcription);
    }
  } else {
    log_error("Dictation failed");
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
      copy_tuple(host->detail, sizeof(host->detail), iter, KEY_DETAIL);
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
    s_error_active = false;
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
      item->settled = int_tuple(iter, KEY_SETTLED, 0) != 0;
      if (index + 1 > s_session_count) {
        s_session_count = index + 1;
      }
    }
  } else if (command == CMD_SESSION_END) {
    int total = int_tuple(iter, KEY_TOTAL, s_session_count);
    if (total == 0) {
      reset_sessions();
    }
    s_scope = int_tuple(iter, KEY_SCOPE, s_scope);
    s_offset = int_tuple(iter, KEY_OFFSET, s_offset);
    s_matched = int_tuple(iter, KEY_MATCHED, s_session_count);
    s_other = int_tuple(iter, KEY_OTHER, 0);
    s_loading_threads = false;
    s_threads_synced = true;
    rebuild_footers();
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
    s_detail_loading = false;
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
  } else if (command == CMD_PROJECT_PREVIEW) {
    /* The phone resolved a dictated name to a path. Show it and wait: nothing
       is created until the confirm menu is answered. */
    copy_tuple(s_pending_project_path, sizeof(s_pending_project_path), iter, KEY_PATH);
    copy_tuple(s_pending_project_name, sizeof(s_pending_project_name), iter, KEY_NAME);
    if (s_pending_project_path[0]) {
      set_status(s_pending_project_path);
      open_project_confirm();
    }
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
    s_detail_loading = false;
    log_error(error[0] ? error : "Error");
    set_status(error[0] ? error : "Error");
    update_detail_text();
    update_stream_timer();
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  char text[48];
  snprintf(text, sizeof(text), "Dropped a message (%d)", (int)reason);
  log_error(text);
  set_status(text);
  mark_all_dirty();
}

static void outbox_failed_callback(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  s_loading_hosts = false;
  s_loading_threads = false;
  s_sending_message = false;
  s_hosts_synced = true;
  s_link_down = true;
  s_detail_loading = false;
  {
    char text[48];
    snprintf(text, sizeof(text), "Phone bridge silent (%d)", (int)reason);
    log_error(text);
    set_status(text);
  }
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
    .select_click = thread_select,
    .select_long_click = thread_select_long
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
  s_font_dseg_big = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_DSEG_42));
  s_font_dseg_small = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_DSEG_20));
  s_font_tech = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_TECH_20));
  s_font_tech_small = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_TECH_16));
  lcd_tiles_create();

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

  s_diag_window = window_create();
  window_set_window_handlers(s_diag_window, (WindowHandlers) {
    .load = diag_window_load,
    .unload = diag_window_unload
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
  if (s_diag_window) {
    window_destroy(s_diag_window);
  }
  lcd_tiles_destroy();
  if (s_font_dseg_big) fonts_unload_custom_font(s_font_dseg_big);
  if (s_font_dseg_small) fonts_unload_custom_font(s_font_dseg_small);
  if (s_font_tech) fonts_unload_custom_font(s_font_tech);
  if (s_font_tech_small) fonts_unload_custom_font(s_font_tech_small);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
