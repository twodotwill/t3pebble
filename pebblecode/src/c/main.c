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

#define MAX_SESSIONS 20
#define MAX_PROJECTS 20
#define MAX_CONTEXT_TEXT 640
#define REFRESH_INTERVAL_MS 300000
#define BUILD_LABEL "v0.1.0"
#define DETAIL_CONTENT_HEIGHT 900
#define DETAIL_CONTENT_MIN_HEIGHT 120
#define STATUS_BAR_HEIGHT 20
#define DETAIL_HEADER_HEIGHT 50
#define PANEL_EDGE 5
#define CARD_PAD 8
#define STREAM_TICK_MS 140
#define RUNNING_TICK_MS 420
#define POLISH_TICKS 5

typedef struct {
  char id[72];
  char title[64];
  char directory[48];
  char agent[24];
  char status[18];
  char summary[180];
  char request_id[72];
  char request_kind[12];
} SessionItem;

typedef struct {
  char id[72];
  char title[64];
  char directory[48];
  char model[24];
} ProjectItem;

typedef enum {
  DictationTargetSession,
  DictationTargetProject
} DictationTarget;

typedef enum {
  ContextPolishNone,
  ContextPolishEnter,
  ContextPolishExit,
  ContextPolishPage,
  ContextPolishNav,
  ContextPolishEdge
} ContextPolishKind;

static Window *s_menu_window;
static Window *s_detail_window;
static MenuLayer *s_menu_layer;
static TextLayer *s_status_layer;
static ScrollLayer *s_scroll_layer;
static Layer *s_detail_header_layer;
static Layer *s_detail_layer;
static DictationSession *s_dictation;
static AppTimer *s_refresh_timer;
static AppTimer *s_stream_timer;

static SessionItem s_sessions[MAX_SESSIONS];
static ProjectItem s_projects[MAX_PROJECTS];
static int s_session_count;
static int s_project_count;
static int s_expected_count;
static int s_selected_index = -1;
static int s_selected_project_index = -1;
static int s_context_page;
static int s_context_page_count = 1;
static int s_pending_context_page;
static int s_pending_context_dir = 1;
static int s_context_request_counter;
static int s_stream_phase;
static int s_menu_select_section = -1;
static int s_menu_select_row = -1;
static int s_menu_select_ticks;
static int s_detail_open_ticks;
static int s_context_polish_ticks;
static int s_context_polish_dir = 1;
static ContextPolishKind s_context_polish_kind;
static bool s_loading;
static bool s_initial_sync = true;
static bool s_showing_context;
static bool s_context_loading;
static bool s_context_page_nav;
static bool s_context_request_active;
static bool s_sending_message;
static int s_scroll_edge_dir;
static int s_scroll_edge_count;
static DictationTarget s_dictation_target;
static char s_status_text[64];
static char s_full_context[MAX_CONTEXT_TEXT];
static char s_pending_context_session_id[72];
static char s_pending_context_request_id[16];
static char s_draw_role[16];
static char s_draw_body[MAX_CONTEXT_TEXT];

static void request_refresh(void);
static void request_detail(int index, bool full_context);
static void request_context_page(int page);
static void send_prompt_text(const char *text);
static void send_new_thread_text(const char *text);
static void start_dictation(void);
static void schedule_refresh(uint32_t delay_ms);
static GColor theme_bg(void);
static GColor theme_panel(void);
static GColor theme_purple(void);
static GColor theme_deep_purple(void);
static GColor theme_bright_purple(void);
static GColor theme_green(void);
static GColor theme_orange(void);
static GColor theme_muted(void);
static GColor theme_text(void);
static GColor theme_selected_text(void);
static GColor theme_cyan(void);
static GColor theme_blue(void);
static GColor theme_red(void);
static GColor theme_card_for_role(const char *role);
static GColor theme_accent_for_role(const char *role);
static void update_stream_timer(void);
static void start_menu_select_polish(int section, int row);
static void start_detail_open_polish(void);
static void start_context_polish(ContextPolishKind kind, int dir);
static void draw_hud_ticks(GContext *ctx, GRect rect, GColor accent, bool active);
static void draw_signal_beacon(GContext *ctx, GRect bounds, GColor accent, bool active);
static void detail_exit_context(bool animated);
static void detail_content_update_proc(Layer *layer, GContext *ctx);

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

static bool status_is_running(const char *status) {
  return strcmp(status, "Running") == 0;
}

static bool has_running_session(void) {
  for (int i = 0; i < s_session_count; i++) {
    if (status_is_running(s_sessions[i].status)) {
      return true;
    }
  }
  return false;
}

static bool selected_session_running(void) {
  return s_selected_index >= 0 && s_selected_index < s_session_count && status_is_running(s_sessions[s_selected_index].status);
}

static bool menu_window_visible(void) {
  return s_menu_window && window_stack_get_top_window() == s_menu_window;
}

static bool detail_window_visible(void) {
  return s_detail_window && window_stack_get_top_window() == s_detail_window;
}

static bool fast_animation_active(void) {
  return (menu_window_visible() && (s_loading || s_sending_message || s_menu_select_ticks > 0)) ||
         (detail_window_visible() && (s_context_loading || s_sending_message || s_detail_open_ticks > 0 || s_context_polish_ticks > 0));
}

static bool slow_animation_active(void) {
  return (menu_window_visible() && has_running_session()) ||
         (detail_window_visible() && selected_session_running());
}

static uint32_t animation_tick_ms(void) {
  if (fast_animation_active()) {
    return STREAM_TICK_MS;
  }
  if (slow_animation_active()) {
    return RUNNING_TICK_MS;
  }
  return 0;
}

static void stream_timer_callback(void *context) {
  s_stream_timer = NULL;
  s_stream_phase = (s_stream_phase + 1) % 12;
  bool menu_polish_active = s_menu_select_ticks > 0;
  bool detail_polish_active = s_detail_open_ticks > 0;
  bool context_polish_active = s_context_polish_ticks > 0;
  if (s_menu_select_ticks > 0) {
    s_menu_select_ticks--;
    if (s_menu_select_ticks == 0) {
      s_menu_select_section = -1;
      s_menu_select_row = -1;
    }
  }
  if (s_detail_open_ticks > 0) {
    s_detail_open_ticks--;
  }
  if (s_context_polish_ticks > 0) {
    s_context_polish_ticks--;
    if (s_context_polish_ticks == 0) {
      s_context_polish_kind = ContextPolishNone;
    }
  }
  bool selected_running = selected_session_running();
  bool menu_active = s_loading || s_sending_message || has_running_session() || menu_polish_active;
  bool detail_header_active = s_context_loading || s_sending_message || selected_running || detail_polish_active || context_polish_active;
  bool detail_content_active = s_context_loading || detail_polish_active || (!s_showing_context && (s_sending_message || selected_running));
  if (menu_active && s_menu_layer && menu_window_visible()) {
    layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
  }
  if (detail_header_active && detail_window_visible() && s_detail_header_layer) {
    layer_mark_dirty(s_detail_header_layer);
  }
  if (detail_content_active && detail_window_visible() && s_detail_layer) {
    layer_mark_dirty(s_detail_layer);
  }
  update_stream_timer();
}

static void update_stream_timer(void) {
  uint32_t tick_ms = animation_tick_ms();
  if (tick_ms > 0) {
    if (!s_stream_timer) {
      s_stream_timer = app_timer_register(tick_ms, stream_timer_callback, NULL);
    }
  } else if (s_stream_timer) {
    app_timer_cancel(s_stream_timer);
    s_stream_timer = NULL;
  }
}

static void start_menu_select_polish(int section, int row) {
  s_menu_select_section = section;
  s_menu_select_row = row;
  s_menu_select_ticks = POLISH_TICKS;
  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
  }
  update_stream_timer();
}

static void start_detail_open_polish(void) {
  s_detail_open_ticks = POLISH_TICKS;
  if (s_detail_header_layer) {
    layer_mark_dirty(s_detail_header_layer);
  }
  if (s_detail_layer) {
    layer_mark_dirty(s_detail_layer);
  }
  update_stream_timer();
}

static int context_polish_ease(int max_px) {
  if (s_context_polish_ticks <= 0 || max_px <= 0) {
    return 0;
  }
  int t = s_context_polish_ticks;
  return (max_px * t * t) / (POLISH_TICKS * POLISH_TICKS);
}

static int context_polish_offset(void) {
  if (s_context_polish_kind == ContextPolishNone || s_context_polish_kind == ContextPolishEdge || s_showing_context) {
    return 0;
  }
  return s_context_polish_dir * context_polish_ease(18);
}

static void start_context_polish(ContextPolishKind kind, int dir) {
  if (dir == 0) {
    dir = 1;
  }
  s_context_polish_kind = kind;
  s_context_polish_dir = dir;
  s_context_polish_ticks = POLISH_TICKS;
  if (s_detail_header_layer) {
    layer_mark_dirty(s_detail_header_layer);
  }
  bool body_polish_visible = !(s_showing_context && (kind == ContextPolishNav || kind == ContextPolishEdge));
  if (body_polish_visible && s_detail_layer) {
    layer_mark_dirty(s_detail_layer);
  }
  update_stream_timer();
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

static void set_status(const char *text) {
  strncpy(s_status_text, text, sizeof(s_status_text) - 1);
  s_status_text[sizeof(s_status_text) - 1] = '\0';
  if (s_status_layer) {
    text_layer_set_text(s_status_layer, s_status_text);
  }
}

static const char *short_path(const char *path) {
  const char *last = strrchr(path, '/');
  if (!last || !last[1]) {
    return path;
  }
  return last + 1;
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

static GFont font_title(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
}

static GFont font_body(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_18);
}

static GFont font_body_bold(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
}

static GFont font_micro(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
}

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
    text,
    font,
    GRect(0, 0, width, 1000),
    GTextOverflowModeWordWrap,
    GTextAlignmentLeft
  );
  return size.h;
}

static int context_card_height(const char *body, int width) {
  int body_width = width - 28;
  int body_h = text_height(body, font_body(), body_width);
  return clamp_int(22 + body_h + 16, 58, 260);
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
    y += context_card_height(s_draw_body, width) + 8;
  }
  return clamp_int(y + CARD_PAD, DETAIL_CONTENT_MIN_HEIGHT, DETAIL_CONTENT_HEIGHT);
}

static void context_visible_y_range(int *visible_top, int *visible_bottom) {
  if (!s_scroll_layer) {
    *visible_top = 0;
    *visible_bottom = DETAIL_CONTENT_HEIGHT;
    return;
  }

  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  *visible_top = -offset.y - CARD_PAD;
  *visible_bottom = -offset.y + scroll_bounds.size.h + CARD_PAD;
}

static int detail_content_height_for_width(int width) {
  if (s_showing_context) {
    return context_content_height(width);
  }

  if (s_selected_index < 0 || s_selected_index >= s_session_count) {
    return DETAIL_CONTENT_MIN_HEIGHT;
  }
  SessionItem *item = &s_sessions[s_selected_index];
  const char *summary = item->summary[0] ? item->summary : "No assistant summary yet.";
  int body_width = width - 28;
  int height = 58 + text_height(summary, font_body(), body_width) + 52;
  return clamp_int(height, DETAIL_CONTENT_MIN_HEIGHT, DETAIL_CONTENT_HEIGHT);
}

static void update_detail_text(void) {
  if (s_selected_index < 0 || s_selected_index >= s_session_count) {
    return;
  }

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

static void reset_sessions(void) {
  memset(s_sessions, 0, sizeof(s_sessions));
  s_session_count = 0;
  s_expected_count = 0;
}

static void reset_projects(void) {
  memset(s_projects, 0, sizeof(s_projects));
  s_project_count = 0;
}

static bool showing_initial_sync(void) {
  return s_initial_sync;
}

static void send_command_begin(DictionaryIterator **iter, int command) {
  if (app_message_outbox_begin(iter) != APP_MSG_OK || !*iter) {
    set_status("Phone link busy");
    return;
  }
  dict_write_int(*iter, KEY_CMD, &command, sizeof(command), true);
}

static void request_refresh(void) {
  if (s_loading) {
    return;
  }
  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, CMD_REFRESH);
  if (!iter) {
    return;
  }
  s_loading = true;
  set_status("Refreshing...");
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

  s_showing_context = full_context;
  s_context_loading = false;
  s_context_page_nav = false;
  s_context_request_active = false;
  s_pending_context_session_id[0] = '\0';
  s_pending_context_request_id[0] = '\0';
  s_context_page = 0;
  s_context_page_count = 1;
  s_full_context[0] = '\0';
  set_status("Loading detail...");
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
  bool was_showing_context = s_showing_context;
  if (page < -1) {
    page = 0;
  }
  if (s_context_page_count > 0 && page >= s_context_page_count) {
    page = s_context_page_count - 1;
  }
  int target_page = page < 0 ? 0 : page;
  int dir = target_page > s_context_page ? 1 : (target_page < s_context_page ? -1 : 1);

  s_showing_context = true;
  s_context_loading = true;
  s_context_request_active = true;
  s_scroll_edge_dir = 0;
  s_scroll_edge_count = 0;
  s_pending_context_page = page;
  s_pending_context_dir = dir;
  strncpy(s_pending_context_session_id, s_sessions[s_selected_index].id, sizeof(s_pending_context_session_id) - 1);
  s_pending_context_session_id[sizeof(s_pending_context_session_id) - 1] = '\0';
  s_context_request_counter = (s_context_request_counter % 9998) + 1;
  snprintf(s_pending_context_request_id, sizeof(s_pending_context_request_id), "c%d", s_context_request_counter);
  s_full_context[0] = '\0';
  set_status("Loading transcript...");
  start_context_polish(was_showing_context ? ContextPolishPage : ContextPolishEnter, dir);
  update_detail_text();

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
  set_status("Sending reply...");
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
  set_status("Starting thread...");
  update_stream_timer();
  app_message_outbox_send();
}

static uint16_t menu_get_num_sections_callback(MenuLayer *menu_layer, void *data) {
  return 2;
}

static uint16_t menu_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  if (section_index == 1) {
    return showing_initial_sync() ? 0 : s_project_count;
  }
  if (showing_initial_sync() || s_session_count == 0) {
    return 1;
  }
  return s_session_count;
}

static int16_t menu_get_header_height_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  return 20;
}

static int16_t menu_get_cell_height_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  return 46;
}

static void menu_draw_header_callback(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *data) {
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_fill_color(ctx, theme_deep_purple());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_bg());
  graphics_fill_rect(ctx, GRect(PANEL_EDGE, 3, bounds.size.w - (PANEL_EDGE * 2), bounds.size.h - 6), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_purple());
  graphics_fill_rect(ctx, GRect(0, 0, PANEL_EDGE, bounds.size.h), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_cyan());
  graphics_fill_rect(ctx, GRect(PANEL_EDGE, bounds.size.h - 2, bounds.size.w - PANEL_EDGE, 2), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_orange());
  graphics_fill_rect(ctx, GRect(bounds.size.w - 18, 0, 18, 3), 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, theme_bright_purple());
  graphics_draw_line(ctx, GPoint(PANEL_EDGE, 2), GPoint(bounds.size.w - 20, 2));
  if (section_index == 0 && (showing_initial_sync() || s_loading)) {
    int pulse_x = PANEL_EDGE + 8 + (s_stream_phase % 24);
    graphics_context_set_fill_color(ctx, theme_cyan());
    graphics_fill_rect(ctx, GRect(pulse_x, bounds.size.h - 4, 14, 2), 0, GCornerNone);
  }
  graphics_context_set_text_color(ctx, theme_cyan());
  graphics_draw_text(ctx, section_index == 1 ? "New Thread" : "T3 Code Agents", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(9, 0, bounds.size.w - 34, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, theme_orange());
  graphics_draw_text(ctx, "T3", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(bounds.size.w - 20, 0, 17, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

static const char *status_prefix(const char *status) {
  if (strcmp(status, "Needs input") == 0) {
    return "?";
  }
  if (strcmp(status, "Running") == 0) {
    return ">";
  }
  if (strcmp(status, "Done") == 0) {
    return "+";
  }
  if (strcmp(status, "Error") == 0) {
    return "!";
  }
  return "-";
}

static GColor theme_bg(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x02060D);
#else
  return GColorBlack;
#endif
}

static GColor theme_panel(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x092033);
#else
  return GColorBlack;
#endif
}

static GColor theme_purple(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x0C3D86);
#else
  return GColorBlack;
#endif
}

static GColor theme_deep_purple(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x07152A);
#else
  return GColorBlack;
#endif
}

static GColor theme_bright_purple(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xFF2D95);
#else
  return GColorBlack;
#endif
}

static GColor theme_green(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xA8FF3D);
#else
  return GColorWhite;
#endif
}

static GColor theme_orange(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xFF8A1A);
#else
  return GColorWhite;
#endif
}

static GColor theme_muted(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xB8D6E8);
#else
  return GColorWhite;
#endif
}

static GColor theme_text(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xF4FBFF);
#else
  return GColorWhite;
#endif
}

static GColor theme_selected_text(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xF8FFFF);
#else
  return GColorWhite;
#endif
}

static GColor theme_cyan(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x18E7FF);
#else
  return GColorWhite;
#endif
}

static GColor theme_blue(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x0A315A);
#else
  return GColorBlack;
#endif
}

static GColor theme_red(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xFF355D);
#else
  return GColorWhite;
#endif
}

static bool role_is(const char *role, const char *match) {
  return strcmp(role, match) == 0;
}

static GColor theme_card_for_role(const char *role) {
  if (role_is(role, "You")) {
    return theme_blue();
  }
#ifdef PBL_COLOR
  if (role_is(role, "Agent")) {
    return GColorFromHEX(0x1D123E);
  }
  if (role_is(role, "Error")) {
    return GColorFromHEX(0x4A0A1B);
  }
  if (role_is(role, "System") || role_is(role, "Note")) {
    return GColorFromHEX(0x3D2504);
  }
#endif
  return theme_deep_purple();
}

static GColor theme_accent_for_role(const char *role) {
  if (role_is(role, "You")) {
    return theme_cyan();
  }
  if (role_is(role, "Agent")) {
    return theme_bright_purple();
  }
  if (role_is(role, "Error")) {
    return theme_red();
  }
  if (role_is(role, "System") || role_is(role, "Note")) {
    return theme_orange();
  }
  return theme_green();
}

static GColor status_color(const char *status) {
#ifdef PBL_COLOR
  if (strcmp(status, "Needs input") == 0) {
    return theme_orange();
  }
  if (strcmp(status, "Running") == 0) {
    return theme_green();
  }
  if (strcmp(status, "Done") == 0) {
    return theme_cyan();
  }
  if (strcmp(status, "Error") == 0) {
    return GColorRed;
  }
#endif
  return GColorWhite;
}

static void draw_scanlines(GContext *ctx, GRect bounds) {
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorFromHEX(0x10152B));
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  int start_y = 4;
  int end_y = bounds.size.h;
  if (s_scroll_layer) {
    int visible_top = 0;
    int visible_bottom = bounds.size.h;
    context_visible_y_range(&visible_top, &visible_bottom);
    start_y = clamp_int(visible_top, 4, bounds.size.h);
    end_y = clamp_int(visible_bottom, start_y, bounds.size.h);
    int remainder = (start_y - 4) % 12;
    if (remainder < 0) {
      remainder += 12;
    }
    if (remainder != 0) {
      start_y += 12 - remainder;
    }
  }
  for (int y = start_y; y < end_y; y += 12) {
    graphics_draw_line(ctx, GPoint(bounds.origin.x, bounds.origin.y + y), GPoint(bounds.origin.x + bounds.size.w, bounds.origin.y + y));
  }
}

static void draw_stream_bars(GContext *ctx, GRect bounds, GColor accent) {
  int rail_y = bounds.origin.y + bounds.size.h - 8;
  int rail_x = bounds.origin.x + 8;
  int rail_w = bounds.size.w - 16;
  int bar_w = 9;
  if (rail_w <= bar_w) {
    return;
  }
  int travel_w = rail_w - bar_w;
  graphics_context_set_fill_color(ctx, theme_deep_purple());
  graphics_fill_rect(ctx, GRect(rail_x, rail_y, rail_w, 3), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, accent);
  for (int i = 0; i < 4; i++) {
    int x = rail_x + ((s_stream_phase * 7) + (i * 17)) % travel_w;
    graphics_fill_rect(ctx, GRect(x, rail_y - 1, bar_w, 5), 0, GCornerNone);
  }
}

static void draw_press_sweep(GContext *ctx, GRect rect, GColor accent, int ticks) {
  if (ticks <= 0) {
    return;
  }
  int elapsed = POLISH_TICKS - ticks;
  int sweep_w = 18;
  int travel_w = rect.size.w - sweep_w - 10;
  if (travel_w < 1) {
    travel_w = 1;
  }
  int x = rect.origin.x + 5 + ((elapsed * travel_w) / (POLISH_TICKS - 1));
  graphics_context_set_fill_color(ctx, accent);
  graphics_fill_rect(ctx, GRect(x, rect.origin.y + 2, sweep_w, 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(x + 5, rect.origin.y + rect.size.h - 4, sweep_w - 5, 2), 0, GCornerNone);
}

static void draw_running_pip(GContext *ctx, GRect rect, GColor accent) {
  int pip_h = 8;
  int travel_h = rect.size.h - pip_h - 8;
  if (travel_h < 1) {
    travel_h = 1;
  }
  int y = rect.origin.y + 4 + ((s_stream_phase * 3) % travel_h);
  graphics_context_set_fill_color(ctx, accent);
  graphics_fill_rect(ctx, GRect(rect.origin.x + rect.size.w - 4, y, 3, pip_h), 0, GCornerNone);
}

static void draw_hud_ticks(GContext *ctx, GRect rect, GColor accent, bool active) {
  int drift = active ? s_stream_phase % 4 : 0;
  graphics_context_set_fill_color(ctx, accent);
  graphics_fill_rect(ctx, GRect(rect.origin.x + 5 + drift, rect.origin.y + 4, 10, 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(rect.origin.x + 5, rect.origin.y + 4, 2, 10 + drift), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(rect.origin.x + rect.size.w - 17 - drift, rect.origin.y + rect.size.h - 6, 12, 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(rect.origin.x + rect.size.w - 7, rect.origin.y + rect.size.h - 16 - drift, 2, 10), 0, GCornerNone);
}

static void draw_signal_beacon(GContext *ctx, GRect bounds, GColor accent, bool active) {
  int pulse = active ? s_stream_phase % 3 : 0;
  int x = bounds.origin.x + bounds.size.w - 17;
  int y = bounds.origin.y + 8;
  graphics_context_set_fill_color(ctx, theme_deep_purple());
  graphics_fill_rect(ctx, GRect(x - 2, y - 2, 13, 13), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, accent);
  graphics_fill_rect(ctx, GRect(x + pulse, y, 7 - pulse, 7), 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, accent);
  graphics_draw_line(ctx, GPoint(x - 4, y + 3), GPoint(x - 2, y + 3));
  graphics_draw_line(ctx, GPoint(x + 9, y + 3), GPoint(x + 12, y + 3));
}

static void draw_page_meter(GContext *ctx, GRect bounds) {
  if (!s_showing_context || s_context_page_count <= 1) {
    return;
  }
  int meter_w = bounds.size.w - 8;
  if (meter_w < 12) {
    return;
  }
  int block_w = s_context_page_nav ? 5 : 4;
  int gap = 2;
  int max_count = (meter_w + gap) / (block_w + gap);
  if (max_count < 2) {
    max_count = 2;
  } else if (max_count > 6) {
    max_count = 6;
  }
  int overflow = s_context_page_count > max_count;
  int count = overflow ? max_count : s_context_page_count;
  int total_w = count * block_w + (count - 1) * gap;
  int x = bounds.origin.x + (bounds.size.w - total_w) / 2;
  int y = bounds.origin.y + 5;
  int window_start = 0;
  if (overflow) {
    window_start = s_context_page - (count / 2);
    if (window_start < 0) {
      window_start = 0;
    }
    if (window_start + count > s_context_page_count) {
      window_start = s_context_page_count - count;
    }
  }
  for (int i = 0; i < count; i++) {
    int page = window_start + i;
    int active = page == s_context_page;
    int edge_overflow = overflow && (i == 0 || i == count - 1) && !active;
    int pulse = active && (s_context_polish_kind == ContextPolishPage || s_context_polish_kind == ContextPolishNav) ? context_polish_ease(3) : 0;
    graphics_context_set_fill_color(ctx, active ? (s_context_page_nav ? theme_orange() : theme_cyan()) : (edge_overflow ? theme_bright_purple() : theme_purple()));
    graphics_fill_rect(ctx, GRect(x + i * (block_w + gap) - pulse / 2, y - (s_context_page_nav && active ? 1 : 0) - pulse / 2, block_w + pulse, (s_context_page_nav ? 5 : 4) + pulse), 0, GCornerNone);
  }
}

static void draw_context_polish_overlay(GContext *ctx, GRect bounds) {
  if (s_context_polish_ticks <= 0) {
    return;
  }

  int glow = context_polish_ease(10);
  GColor accent = s_context_polish_kind == ContextPolishEdge ? theme_orange() : theme_cyan();
  graphics_context_set_fill_color(ctx, accent);

  if (s_context_polish_kind == ContextPolishEdge) {
    int y = s_context_polish_dir < 0 ? 4 + glow / 2 : bounds.size.h - 9 - glow / 2;
    int center = bounds.size.w / 2;
    graphics_fill_rect(ctx, GRect(center - 16, y, 32, 2), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(center - 8, y + (s_context_polish_dir < 0 ? 4 : -4), 16, 2), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(center - 3, y + (s_context_polish_dir < 0 ? 8 : -8), 6, 2), 0, GCornerNone);
    return;
  }

  int rail_x = s_context_polish_dir > 0 ? bounds.size.w - 5 - glow / 3 : 2 + glow / 3;
  graphics_fill_rect(ctx, GRect(rail_x, 10, 3, bounds.size.h - 20), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(rail_x + (s_context_polish_dir > 0 ? -8 : 4), 18 + glow, 8, 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(rail_x + (s_context_polish_dir > 0 ? -12 : 4), bounds.size.h - 24 - glow, 12, 2), 0, GCornerNone);
}

static void draw_card_frame(GContext *ctx, GRect card, GColor accent) {
  graphics_context_set_fill_color(ctx, accent);
  graphics_fill_rect(ctx, GRect(card.origin.x, card.origin.y, 4, card.size.h), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(card.origin.x, card.origin.y, card.size.w, 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(card.origin.x + card.size.w - 22, card.origin.y + card.size.h - 3, 18, 3), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(card.origin.x + 7, card.origin.y + 7, 2, 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(card.origin.x + card.size.w - 12, card.origin.y + card.size.h - 9, 2, 2), 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, accent);
  graphics_draw_line(ctx, GPoint(card.origin.x + card.size.w - 14, card.origin.y + 2), GPoint(card.origin.x + card.size.w - 2, card.origin.y + 14));
  graphics_draw_line(ctx, GPoint(card.origin.x + 5, card.origin.y + card.size.h - 10), GPoint(card.origin.x + 13, card.origin.y + card.size.h - 2));
}

static int draw_context_card(GContext *ctx, int y, int width, int x_offset, const char *role, const char *body, int card_h) {
  int body_width = width - 28;
  GRect card = GRect(6 + x_offset, y, width - 12, card_h);
  GColor accent = theme_accent_for_role(role);
  graphics_context_set_fill_color(ctx, theme_card_for_role(role));
  graphics_fill_rect(ctx, card, 0, GCornerNone);
  draw_card_frame(ctx, card, accent);

  graphics_context_set_text_color(ctx, accent);
  graphics_draw_text(ctx, role[0] ? role : "Signal", font_micro(),
                     GRect(card.origin.x + 10, card.origin.y + 1, card.size.w - 18, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, theme_text());
  graphics_draw_text(ctx, body, font_body(),
                     GRect(card.origin.x + 10, card.origin.y + 22, body_width, card_h - 28),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  return y + card_h + 8;
}

static void draw_context_page(GContext *ctx, GRect bounds, int x_offset) {
  if (s_context_loading || !s_full_context[0]) {
    char loading[32];
    int dots = s_stream_phase % 4;
    snprintf(loading, sizeof(loading), "Streaming%s", dots == 0 ? "" : (dots == 1 ? "." : (dots == 2 ? ".." : "...")));
    graphics_context_set_text_color(ctx, theme_cyan());
    graphics_draw_text(ctx, loading, font_body_bold(),
                       GRect(12 + x_offset, 24, bounds.size.w - 24, 30),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    draw_stream_bars(ctx, GRect(8 + x_offset, 62, bounds.size.w - 16, 30), theme_cyan());
    return;
  }

  int y = CARD_PAD;
  int pos = 0;
  int block_start = 0;
  int block_len = 0;
  int visible_top = 0;
  int visible_bottom = DETAIL_CONTENT_HEIGHT;
  int context_len = strlen(s_full_context);
  context_visible_y_range(&visible_top, &visible_bottom);
  while (pos < context_len) {
    pos = next_context_block_len(s_full_context, context_len, pos, &block_start, &block_len);
    if (block_len <= 0) {
      continue;
    }
    int body_start = role_body_start(s_full_context + block_start, block_len);
    int body_len = block_len - body_start;
    block_role(s_draw_role, sizeof(s_draw_role), s_full_context + block_start, block_len);
    if (body_len >= (int)sizeof(s_draw_body)) {
      body_len = sizeof(s_draw_body) - 1;
    }
    memcpy(s_draw_body, s_full_context + block_start + body_start, body_len);
    s_draw_body[body_len] = '\0';
    int card_h = context_card_height(s_draw_body, bounds.size.w);
    if (y > visible_bottom) {
      break;
    }
    if (y + card_h >= visible_top) {
      draw_context_card(ctx, y, bounds.size.w, x_offset, s_draw_role, s_draw_body, card_h);
    }
    y += card_h + 8;
  }
}

static void detail_content_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, theme_bg());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  draw_scanlines(ctx, bounds);

  if (s_selected_index < 0 || s_selected_index >= s_session_count) {
    return;
  }

  if (s_detail_open_ticks > 0) {
    draw_press_sweep(ctx, GRect(6, 4, bounds.size.w - 12, 24), theme_cyan(), s_detail_open_ticks);
  }

  int x_offset = context_polish_offset();
  if (s_showing_context) {
    draw_context_page(ctx, bounds, x_offset);
    return;
  }

  SessionItem *item = &s_sessions[s_selected_index];
  const char *summary = item->summary[0] ? item->summary : "No assistant summary yet.";
  GRect card = GRect(6 + x_offset, 8, bounds.size.w - 12, bounds.size.h - 16);
  bool needs_input = strcmp(item->status, "Needs input") == 0;
  bool active_status = s_sending_message || selected_session_running();
  graphics_context_set_fill_color(ctx, theme_panel());
  graphics_fill_rect(ctx, card, 0, GCornerNone);
  draw_card_frame(ctx, card, status_color(item->status));
  if (needs_input) {
    draw_signal_beacon(ctx, card, theme_orange(), true);
  }
  if (active_status) {
    draw_hud_ticks(ctx, card, s_sending_message ? theme_orange() : status_color(item->status), true);
    draw_running_pip(ctx, card, s_sending_message ? theme_orange() : status_color(item->status));
  }

  graphics_context_set_text_color(ctx, theme_cyan());
  graphics_draw_text(ctx, "Latest signal", font_body_bold(),
                     GRect(16 + x_offset, 10, bounds.size.w - 32, 26),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, theme_text());
  graphics_draw_text(ctx, summary, font_body(),
                     GRect(16 + x_offset, 42, bounds.size.w - 32, bounds.size.h - 88),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, theme_muted());
  graphics_fill_rect(ctx, GRect(16 + x_offset, bounds.size.h - 23, bounds.size.w - 32, 2), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, status_color(item->status));
  graphics_fill_rect(ctx, GRect(16 + x_offset, bounds.size.h - 27, 24, 6), 0, GCornerNone);
  draw_context_polish_overlay(ctx, bounds);
}

static void menu_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
  GRect bounds = layer_get_bounds(cell_layer);

  if (cell_index->section == 1) {
    if (cell_index->row >= s_project_count) {
      return;
    }
    ProjectItem *project = &s_projects[cell_index->row];
    char title[88];
    char subtitle[88];
    snprintf(title, sizeof(title), "+ %s", project->title[0] ? project->title : "New thread");
    snprintf(subtitle, sizeof(subtitle), "%s | %s",
             project->model[0] ? project->model : "codex",
             project->directory[0] ? short_path(project->directory) : "-");

	    bool selected = menu_layer_is_index_selected(s_menu_layer, cell_index);
	    bool pressed = s_menu_select_ticks > 0 && s_menu_select_section == (int)cell_index->section && s_menu_select_row == (int)cell_index->row;
	    graphics_context_set_fill_color(ctx, theme_bg());
	    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

	    GRect panel = GRect(6, 3, bounds.size.w - 10, bounds.size.h - 5);
	    GRect text_rect = GRect(29, 3, bounds.size.w - 64, 20);
	    GRect sub_rect = GRect(29, 25, bounds.size.w - 64, 17);
		    graphics_context_set_fill_color(ctx, selected ? theme_purple() : (cell_index->row % 2 ? theme_panel() : theme_deep_purple()));
		    graphics_fill_rect(ctx, panel, 0, GCornerNone);

	    if (!selected) {
	      graphics_context_set_fill_color(ctx, cell_index->row % 2 ? theme_deep_purple() : theme_panel());
	      graphics_fill_rect(ctx, GRect(panel.origin.x + 4, panel.origin.y + 3, panel.size.w - 8, panel.size.h - 6), 0, GCornerNone);
	    }
    bool sending_here = s_sending_message && cell_index->row == s_selected_project_index;
    draw_hud_ticks(ctx, panel, sending_here ? theme_orange() : (selected ? theme_cyan() : theme_purple()), selected || sending_here);
    draw_press_sweep(ctx, panel, sending_here ? theme_orange() : theme_cyan(), pressed ? s_menu_select_ticks : 0);

    graphics_context_set_fill_color(ctx, theme_cyan());
    graphics_fill_rect(ctx, GRect(0, 0, PANEL_EDGE, bounds.size.h), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, selected ? theme_cyan() : theme_deep_purple());
    graphics_fill_rect(ctx, GRect(9, 5, 13, 13), 0, GCornerNone);
    graphics_context_set_text_color(ctx, selected ? GColorBlack : theme_cyan());
    graphics_draw_text(ctx, "+", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(9, 2, 13, 16), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    if (selected) {
      graphics_context_set_fill_color(ctx, theme_cyan());
      graphics_fill_rect(ctx, GRect(PANEL_EDGE, 0, bounds.size.w - PANEL_EDGE, 2), 0, GCornerNone);
      graphics_context_set_fill_color(ctx, theme_bright_purple());
      graphics_fill_rect(ctx, GRect(23, 4, 3, bounds.size.h - 8), 0, GCornerNone);
    } else {
      graphics_context_set_stroke_color(ctx, theme_purple());
      graphics_draw_line(ctx, GPoint(6, 3), GPoint(bounds.size.w - 16, 3));
    }

		    graphics_context_set_text_color(ctx, theme_text());
		    graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
		                       text_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
		    graphics_context_set_text_color(ctx, selected ? theme_selected_text() : theme_muted());
		    graphics_draw_text(ctx, subtitle, fonts_get_system_font(FONT_KEY_GOTHIC_14),
		                       sub_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    graphics_context_set_stroke_color(ctx, selected ? theme_cyan() : theme_deep_purple());
    graphics_draw_line(ctx, GPoint(PANEL_EDGE, bounds.size.h - 1), GPoint(bounds.size.w, bounds.size.h - 1));
    return;
  }

	  if (showing_initial_sync() || s_session_count == 0) {
	    GRect sync_panel = GRect(6, 4, bounds.size.w - 12, bounds.size.h - 7);
	    GRect sync_text = GRect(10, 3, bounds.size.w - 58, 20);
	    GRect sync_sub = GRect(10, 25, bounds.size.w - 58, 17);
	    GRect sync_rail = GRect(bounds.size.w - 48, 31, 42, 12);
	    graphics_context_set_fill_color(ctx, theme_bg());
	    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_fill_color(ctx, theme_panel());
    graphics_fill_rect(ctx, sync_panel, 0, GCornerNone);
    draw_hud_ticks(ctx, sync_panel, theme_cyan(), s_loading || showing_initial_sync());
    graphics_context_set_fill_color(ctx, theme_purple());
    graphics_fill_rect(ctx, GRect(0, 0, PANEL_EDGE, bounds.size.h), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, theme_cyan());
    graphics_fill_rect(ctx, GRect(PANEL_EDGE, 0, bounds.size.w - PANEL_EDGE, 2), 0, GCornerNone);
	    graphics_context_set_fill_color(ctx, theme_orange());
	    graphics_fill_rect(ctx, GRect(bounds.size.w - 6, 4, 3, bounds.size.h - 9), 0, GCornerNone);
	    if (s_loading) {
	      draw_stream_bars(ctx, sync_rail, theme_cyan());
	    }
	    graphics_context_set_text_color(ctx, theme_cyan());
	    graphics_draw_text(ctx, showing_initial_sync() || s_loading ? "Syncing agents" : "No agents", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
	                       sync_text, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
	    graphics_context_set_text_color(ctx, theme_muted());
	    graphics_draw_text(ctx, showing_initial_sync() ? "Linking T3 Code" : (s_loading ? "Agent stream" : "Standby"), fonts_get_system_font(FONT_KEY_GOTHIC_14),
	                       sync_sub, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }

  if (cell_index->row >= s_session_count) {
    return;
  }

  SessionItem *item = &s_sessions[cell_index->row];
  char title[88];
  char subtitle[88];
  snprintf(title, sizeof(title), "%s", item->title[0] ? item->title : "Untitled");
  snprintf(subtitle, sizeof(subtitle), "%s | %s | %s",
           item->status[0] ? item->status : "-",
           item->agent[0] ? item->agent : "agent",
           item->directory[0] ? short_path(item->directory) : "-");

	  bool selected = menu_layer_is_index_selected(s_menu_layer, cell_index);
	  bool pressed = s_menu_select_ticks > 0 && s_menu_select_section == (int)cell_index->section && s_menu_select_row == (int)cell_index->row;
	  graphics_context_set_fill_color(ctx, theme_bg());
	  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

	  GRect panel = GRect(6, 3, bounds.size.w - 10, bounds.size.h - 5);
	  GRect text_rect = GRect(29, 3, bounds.size.w - 64, 20);
	  GRect sub_rect = GRect(29, 25, bounds.size.w - 64, 17);
	  GRect beacon_rect = GRect(bounds.size.w - 31, 0, 25, bounds.size.h);
	  graphics_context_set_fill_color(ctx, selected ? theme_purple() : (cell_index->row % 2 ? theme_panel() : theme_deep_purple()));
	  graphics_fill_rect(ctx, panel, 0, GCornerNone);

	  if (!selected) {
	    graphics_context_set_fill_color(ctx, cell_index->row % 2 ? theme_deep_purple() : theme_panel());
	    graphics_fill_rect(ctx, GRect(panel.origin.x + 4, panel.origin.y + 3, panel.size.w - 8, panel.size.h - 6), 0, GCornerNone);
	  }
  bool needs_input = strcmp(item->status, "Needs input") == 0;
  bool is_running = status_is_running(item->status);
  bool is_sending = s_sending_message && cell_index->row == s_selected_index;
  draw_hud_ticks(ctx, panel, is_sending ? theme_orange() : (selected ? theme_cyan() : status_color(item->status)), selected || is_running || is_sending);
  draw_press_sweep(ctx, panel, is_sending ? theme_orange() : status_color(item->status), pressed ? s_menu_select_ticks : 0);

  graphics_context_set_fill_color(ctx, status_color(item->status));
  graphics_fill_rect(ctx, GRect(0, 0, PANEL_EDGE, bounds.size.h), 0, GCornerNone);

  graphics_context_set_fill_color(ctx, selected ? theme_cyan() : theme_deep_purple());
  graphics_fill_rect(ctx, GRect(9, 5, 13, 13), 0, GCornerNone);
  graphics_context_set_text_color(ctx, selected ? GColorBlack : status_color(item->status));
  graphics_draw_text(ctx, status_prefix(item->status), fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(9, 2, 13, 16), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  if (needs_input || strcmp(item->status, "Error") == 0) {
    graphics_context_set_fill_color(ctx, theme_orange());
    graphics_fill_rect(ctx, GRect(bounds.size.w - 5, 3, 4, bounds.size.h - 6), 0, GCornerNone);
	  }
  if (is_running) {
    draw_running_pip(ctx, bounds, status_color(item->status));
  }
	  if (needs_input) {
	    draw_signal_beacon(ctx, beacon_rect, theme_orange(), selected);
	  }

  if (selected) {
    graphics_context_set_fill_color(ctx, theme_cyan());
    graphics_fill_rect(ctx, GRect(PANEL_EDGE, 0, bounds.size.w - PANEL_EDGE, 2), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, theme_bright_purple());
    graphics_fill_rect(ctx, GRect(23, 4, 3, bounds.size.h - 8), 0, GCornerNone);
  } else {
    graphics_context_set_stroke_color(ctx, theme_purple());
    graphics_draw_line(ctx, GPoint(6, 3), GPoint(bounds.size.w - 16, 3));
  }

		  graphics_context_set_text_color(ctx, theme_text());
		  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
		                     text_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
		  graphics_context_set_text_color(ctx, needs_input && !selected ? theme_orange() : (selected ? theme_selected_text() : theme_muted()));
		  graphics_draw_text(ctx, subtitle, fonts_get_system_font(FONT_KEY_GOTHIC_14),
		                     sub_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  graphics_context_set_stroke_color(ctx, selected ? theme_cyan() : theme_deep_purple());
  graphics_draw_line(ctx, GPoint(PANEL_EDGE, bounds.size.h - 1), GPoint(bounds.size.w, bounds.size.h - 1));
}

static void detail_header_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, theme_deep_purple());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_bg());
  graphics_fill_rect(ctx, GRect(6, 5, bounds.size.w - 12, bounds.size.h - 10), 0, GCornerNone);

  if (s_selected_index < 0 || s_selected_index >= s_session_count) {
    return;
  }

  SessionItem *item = &s_sessions[s_selected_index];
  bool needs_input = strcmp(item->status, "Needs input") == 0;
  GRect text_zone = GRect(16, 5, bounds.size.w - 70, bounds.size.h - 10);
  GRect right_bay = GRect(bounds.size.w - 48, 5, 42, bounds.size.h - 10);
  GRect mode_chip = GRect(bounds.size.w - 44, 8, 34, 16);
  GRect meter_slot = GRect(bounds.size.w - 48, 31, 42, 12);
  GRect rail_slot = GRect(bounds.size.w - 48, 31, 42, 12);
  graphics_context_set_fill_color(ctx, status_color(item->status));
  graphics_fill_rect(ctx, GRect(0, 0, PANEL_EDGE, bounds.size.h), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_purple());
  graphics_fill_rect(ctx, GRect(PANEL_EDGE, 0, bounds.size.w - PANEL_EDGE, 3), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_cyan());
  graphics_fill_rect(ctx, GRect(PANEL_EDGE, bounds.size.h - 2, bounds.size.w - PANEL_EDGE, 2), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_bright_purple());
  graphics_fill_rect(ctx, GRect(9, 6, 3, bounds.size.h - 14), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, s_context_page_nav ? theme_orange() : theme_deep_purple());
  graphics_fill_rect(ctx, right_bay, 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, s_context_page_nav ? theme_orange() : theme_purple());
  graphics_draw_line(ctx, GPoint(right_bay.origin.x, right_bay.origin.y), GPoint(right_bay.origin.x + 10, right_bay.origin.y + 10));
  draw_press_sweep(ctx, GRect(6, 5, bounds.size.w - 12, bounds.size.h - 10), theme_cyan(), s_detail_open_ticks);

  if (needs_input || strcmp(item->status, "Error") == 0) {
    graphics_context_set_fill_color(ctx, theme_orange());
    graphics_fill_rect(ctx, GRect(bounds.size.w - 5, 6, 4, bounds.size.h - 14), 0, GCornerNone);
  } else if (selected_session_running()) {
    graphics_context_set_fill_color(ctx, status_color(item->status));
    graphics_fill_rect(ctx, GRect(bounds.size.w - 5, 6 + (s_stream_phase % 4), 4, bounds.size.h - 14), 0, GCornerNone);
  }

  graphics_context_set_text_color(ctx, theme_text());
  graphics_draw_text(ctx, item->title[0] ? item->title : "Untitled",
                     font_title(),
                     GRect(text_zone.origin.x, text_zone.origin.y - 1, text_zone.size.w, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  char status_line[64];
  snprintf(status_line, sizeof(status_line), "%s", item->status[0] ? item->status : "-");
  graphics_context_set_text_color(ctx, s_context_page_nav ? theme_orange() : (needs_input ? theme_orange() : status_color(item->status)));
  graphics_draw_text(ctx, status_line, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(text_zone.origin.x, text_zone.origin.y + 22, text_zone.size.w, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  const char *mode = s_sending_message ? "SND" : (s_showing_context ? (s_context_page_nav ? "PG" : "SCR") : "SUM");
  graphics_context_set_text_color(ctx, s_sending_message ? theme_orange() : (s_context_page_nav ? theme_orange() : theme_cyan()));
  graphics_draw_text(ctx, mode, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     mode_chip,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  if (s_sending_message) {
    draw_stream_bars(ctx, rail_slot, theme_orange());
  } else if (s_context_loading) {
    draw_stream_bars(ctx, rail_slot, theme_cyan());
  } else if (selected_session_running()) {
    draw_stream_bars(ctx, rail_slot, status_color(item->status));
  } else {
    draw_page_meter(ctx, meter_slot);
  }
}

static void menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  if (cell_index->section == 1) {
    if (cell_index->row >= s_project_count) {
      return;
    }
    start_menu_select_polish(cell_index->section, cell_index->row);
    s_selected_project_index = cell_index->row;
    s_dictation_target = DictationTargetProject;
    set_status("Dictate first prompt");
    start_dictation();
    return;
  }

  if (showing_initial_sync() || s_session_count == 0) {
    start_menu_select_polish(cell_index->section, cell_index->row);
    request_refresh();
    return;
  }
  if (cell_index->row >= s_session_count) {
    return;
  }
  start_menu_select_polish(cell_index->section, cell_index->row);
  s_selected_index = cell_index->row;
  s_showing_context = false;
  s_context_loading = false;
  s_context_page_nav = false;
  s_context_request_active = false;
  s_pending_context_session_id[0] = '\0';
  s_pending_context_request_id[0] = '\0';
  s_full_context[0] = '\0';
  window_stack_push(s_detail_window, true);
  start_detail_open_polish();
  update_detail_text();
  request_detail(s_selected_index, false);
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

static void detail_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_showing_context) {
    s_context_page_nav = !s_context_page_nav;
    s_scroll_edge_dir = 0;
    s_scroll_edge_count = 0;
    start_context_polish(ContextPolishNav, s_context_page_nav ? 1 : -1);
  } else {
    request_detail(s_selected_index, true);
  }
}

static void detail_select_long_handler(ClickRecognizerRef recognizer, void *context) {
  s_dictation_target = DictationTargetSession;
  start_dictation();
}

static void detail_reset_scroll(bool animated) {
  if (s_scroll_layer) {
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, 0), animated);
  }
  s_scroll_edge_dir = 0;
  s_scroll_edge_count = 0;
}

static void detail_exit_context(bool animated) {
  s_showing_context = false;
  s_context_loading = false;
  s_context_page_nav = false;
  s_context_request_active = false;
  s_context_page = 0;
  s_context_page_count = 1;
  s_full_context[0] = '\0';
  detail_reset_scroll(false);
  update_detail_text();
  if (animated) {
    start_context_polish(ContextPolishExit, -1);
  } else {
    update_stream_timer();
  }
}

static void detail_back_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_showing_context || s_context_loading) {
    detail_exit_context(true);
    return;
  }
  window_stack_pop(true);
}

static void detail_scroll_to_edge(bool bottom) {
  if (!s_scroll_layer) {
    return;
  }
  GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  int content_h = s_detail_layer ? layer_get_frame(s_detail_layer).size.h : detail_content_height_for_width(scroll_bounds.size.w);
  int max_offset = content_h - scroll_bounds.size.h;
  if (max_offset < 0) {
    max_offset = 0;
  }
  scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, bottom ? -max_offset : 0), true);
  s_scroll_edge_dir = 0;
  s_scroll_edge_count = 0;
}

static int detail_scroll_max_offset(void) {
  if (!s_scroll_layer) {
    return 0;
  }
  GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  int content_h = s_detail_layer ? layer_get_frame(s_detail_layer).size.h : detail_content_height_for_width(scroll_bounds.size.w);
  int max_offset = content_h - scroll_bounds.size.h;
  return max_offset > 0 ? max_offset : 0;
}

static bool detail_scroll_at_top(void) {
  if (!s_scroll_layer) {
    return true;
  }
  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  return offset.y >= -2;
}

static bool detail_scroll_at_bottom(void) {
  if (!s_scroll_layer) {
    return true;
  }
  int max_offset = detail_scroll_max_offset();
  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  return offset.y <= -(max_offset - 2);
}

static bool detail_maybe_page_from_scroll_edge(int dir) {
  if (!s_showing_context || s_context_page_nav || s_context_loading || s_context_page_count <= 1) {
    s_scroll_edge_dir = 0;
    s_scroll_edge_count = 0;
    return false;
  }
  if (dir < 0 && !detail_scroll_at_top()) {
    s_scroll_edge_dir = 0;
    s_scroll_edge_count = 0;
    return false;
  }
  if (dir > 0 && !detail_scroll_at_bottom()) {
    s_scroll_edge_dir = 0;
    s_scroll_edge_count = 0;
    return false;
  }
  if ((dir < 0 && s_context_page <= 0) || (dir > 0 && s_context_page + 1 >= s_context_page_count)) {
    s_scroll_edge_dir = 0;
    s_scroll_edge_count = 0;
    start_context_polish(ContextPolishEdge, dir);
    return true;
  }

  if (s_scroll_edge_dir == dir) {
    s_scroll_edge_count++;
  } else {
    s_scroll_edge_dir = dir;
    s_scroll_edge_count = 1;
  }

  if (s_scroll_edge_count >= 2) {
    s_scroll_edge_dir = 0;
    s_scroll_edge_count = 0;
    request_context_page(s_context_page + dir);
  } else {
    start_context_polish(ContextPolishEdge, dir);
  }
  return true;
}

static void detail_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_context_loading) {
    return;
  }
  if (s_scroll_layer) {
    if (s_showing_context && s_context_page_nav && s_context_page > 0) {
      request_context_page(s_context_page - 1);
      return;
    }
    if (detail_maybe_page_from_scroll_edge(-1)) {
      return;
    }
    s_scroll_edge_dir = 0;
    s_scroll_edge_count = 0;
    scroll_layer_scroll_up_click_handler(recognizer, s_scroll_layer);
  }
}

static void detail_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_context_loading) {
    return;
  }
  if (s_scroll_layer) {
    if (s_showing_context && s_context_page_nav && s_context_page + 1 < s_context_page_count) {
      request_context_page(s_context_page + 1);
      return;
    }
    if (detail_maybe_page_from_scroll_edge(1)) {
      return;
    }
    s_scroll_edge_dir = 0;
    s_scroll_edge_count = 0;
    scroll_layer_scroll_down_click_handler(recognizer, s_scroll_layer);
  }
}

static void detail_up_long_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_context_loading) {
    return;
  }
  if (s_showing_context && s_context_page_nav && s_context_page > 0) {
    request_context_page(s_context_page - 5);
    return;
  }
  detail_scroll_to_edge(false);
}

static void detail_down_long_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_context_loading) {
    return;
  }
  if (s_showing_context && s_context_page_nav && s_context_page + 1 < s_context_page_count) {
    request_context_page(s_context_page + 5);
    return;
  }
  detail_scroll_to_edge(true);
}

static void detail_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_BACK, detail_back_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, detail_select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, detail_select_long_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, detail_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, detail_down_click_handler);
  window_long_click_subscribe(BUTTON_ID_UP, 500, detail_up_long_handler, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 500, detail_down_long_handler, NULL);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120, detail_up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120, detail_down_click_handler);
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  int command = int_tuple(iter, KEY_CMD, 0);

  if (command == CMD_SESSION_ITEM) {
    int index = int_tuple(iter, KEY_INDEX, -1);
    int total = int_tuple(iter, KEY_TOTAL, 0);
    if (index == 0) {
      reset_sessions();
    }
    if (index >= 0 && index < MAX_SESSIONS) {
      SessionItem *item = &s_sessions[index];
      copy_tuple(item->id, sizeof(item->id), iter, KEY_SESSION_ID);
      copy_tuple(item->title, sizeof(item->title), iter, KEY_TITLE);
      copy_tuple(item->directory, sizeof(item->directory), iter, KEY_DIRECTORY);
      copy_tuple(item->agent, sizeof(item->agent), iter, KEY_AGENT);
      copy_tuple(item->status, sizeof(item->status), iter, KEY_STATUS);
      copy_tuple(item->summary, sizeof(item->summary), iter, KEY_SUMMARY);
      copy_tuple(item->request_id, sizeof(item->request_id), iter, KEY_REQUEST_ID);
      copy_tuple(item->request_kind, sizeof(item->request_kind), iter, KEY_REQUEST_KIND);
      if (index + 1 > s_session_count) {
        s_session_count = index + 1;
      }
    }
    s_expected_count = total;
  } else if (command == CMD_SESSION_END) {
    s_loading = false;
    update_stream_timer();
    s_initial_sync = false;
    int total = int_tuple(iter, KEY_TOTAL, s_session_count);
    if (total == 0) {
      reset_sessions();
    }
    if (s_session_count > 0) {
      char status[64];
      snprintf(status, sizeof(status), "%d agent%s loaded", s_session_count, s_session_count == 1 ? "" : "s");
      set_status(status);
    } else {
      set_status("No open sessions");
    }
    schedule_refresh(REFRESH_INTERVAL_MS);
    menu_layer_reload_data(s_menu_layer);
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
      copy_tuple(project->model, sizeof(project->model), iter, KEY_AGENT);
      if (index + 1 > s_project_count) {
        s_project_count = index + 1;
      }
    }
  } else if (command == CMD_PROJECT_END) {
    int total = int_tuple(iter, KEY_TOTAL, s_project_count);
    if (total == 0) {
      reset_projects();
    }
    menu_layer_reload_data(s_menu_layer);
  } else if (command == CMD_DETAIL) {
    if (s_showing_context || s_context_loading) {
      return;
    }
    int index = int_tuple(iter, KEY_INDEX, s_selected_index);
    char detail_response_id[72] = "";
    copy_tuple(detail_response_id, sizeof(detail_response_id), iter, KEY_SESSION_ID);
    if (s_selected_index < 0 || s_selected_index >= s_session_count || index != s_selected_index || strcmp(detail_response_id, s_sessions[s_selected_index].id) != 0) {
      return;
    }
    if (index >= 0 && index < s_session_count) {
      copy_tuple(s_sessions[index].summary, sizeof(s_sessions[index].summary), iter, KEY_SUMMARY);
      copy_tuple(s_sessions[index].status, sizeof(s_sessions[index].status), iter, KEY_STATUS);
      copy_tuple(s_sessions[index].request_id, sizeof(s_sessions[index].request_id), iter, KEY_REQUEST_ID);
      copy_tuple(s_sessions[index].request_kind, sizeof(s_sessions[index].request_kind), iter, KEY_REQUEST_KIND);
      s_showing_context = false;
      s_context_loading = false;
      s_context_page_nav = false;
      s_context_request_active = false;
      s_pending_context_session_id[0] = '\0';
      s_pending_context_request_id[0] = '\0';
      update_stream_timer();
      s_context_page = 0;
      s_context_page_count = 1;
      update_detail_text();
      menu_layer_reload_data(s_menu_layer);
    }
  } else if (command == CMD_CONTEXT) {
    if (!s_context_request_active) {
      s_context_loading = false;
      update_stream_timer();
      return;
    }
    char response_request_id[16] = "";
    copy_tuple(response_request_id, sizeof(response_request_id), iter, KEY_REQUEST_ID);
    if (strcmp(response_request_id, s_pending_context_request_id) != 0) {
      return;
    }
    if (s_selected_index < 0 || s_selected_index >= s_session_count) {
      detail_exit_context(false);
      return;
    }
    int index = int_tuple(iter, KEY_INDEX, s_selected_index);
    char response_id[72] = "";
    copy_tuple(response_id, sizeof(response_id), iter, KEY_SESSION_ID);
    if (index != s_selected_index || strcmp(response_id, s_pending_context_session_id) != 0 || strcmp(response_id, s_sessions[s_selected_index].id) != 0) {
      detail_exit_context(false);
      set_status("Stale transcript");
      return;
    }
    s_context_loading = false;
    s_context_request_active = false;
    s_pending_context_session_id[0] = '\0';
    s_pending_context_request_id[0] = '\0';
    update_stream_timer();
    s_context_page = int_tuple(iter, KEY_CONTEXT_PAGE, s_pending_context_page);
    s_context_page_count = int_tuple(iter, KEY_TOTAL, 1);
    if (s_context_page_count < 1) {
      s_context_page_count = 1;
    }
    copy_tuple(s_full_context, sizeof(s_full_context), iter, KEY_CONTEXT);
    s_showing_context = true;
    update_detail_text();
    if (s_scroll_layer) {
      scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, 0), false);
    }
  } else if (command == CMD_PROMPT) {
    s_sending_message = false;
    update_stream_timer();
    set_status("Reply sent");
    request_refresh();
  } else if (command == CMD_ERROR) {
    char error[120] = "Phone bridge error";
    copy_tuple(error, sizeof(error), iter, KEY_ERROR);
    set_status(error);
    s_loading = false;
    s_sending_message = false;
    if (s_showing_context && !s_full_context[0]) {
      detail_exit_context(false);
    } else {
      s_context_loading = false;
      s_context_request_active = false;
      s_pending_context_session_id[0] = '\0';
      s_pending_context_request_id[0] = '\0';
      update_detail_text();
    }
    update_stream_timer();
    s_initial_sync = false;
    schedule_refresh(REFRESH_INTERVAL_MS);
    menu_layer_reload_data(s_menu_layer);
  } else if (command == CMD_STATUS) {
    char status[64] = "Phone bridge";
    copy_tuple(status, sizeof(status), iter, KEY_STATUS);
    set_status(status);
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  s_sending_message = false;
  if (s_showing_context && !s_full_context[0]) {
    detail_exit_context(false);
  } else {
    s_context_loading = false;
    s_context_request_active = false;
    s_pending_context_session_id[0] = '\0';
    s_pending_context_request_id[0] = '\0';
    update_detail_text();
  }
  update_stream_timer();
  set_status("Message dropped");
}

static void outbox_failed_callback(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  s_loading = false;
  s_sending_message = false;
  s_initial_sync = false;
  if (s_showing_context && !s_full_context[0]) {
    detail_exit_context(false);
  } else {
    s_context_loading = false;
    s_context_request_active = false;
    s_pending_context_session_id[0] = '\0';
    s_pending_context_request_id[0] = '\0';
    update_detail_text();
  }
  update_stream_timer();
  set_status("Phone send failed");
  schedule_refresh(REFRESH_INTERVAL_MS);
}

static void menu_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  window_set_background_color(window, theme_bg());

  s_status_layer = text_layer_create(GRect(0, 0, bounds.size.w, STATUS_BAR_HEIGHT));
  text_layer_set_background_color(s_status_layer, theme_deep_purple());
  text_layer_set_text_color(s_status_layer, theme_cyan());
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_text(s_status_layer, s_status_text);
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  s_menu_layer = menu_layer_create(GRect(0, STATUS_BAR_HEIGHT, bounds.size.w, bounds.size.h - STATUS_BAR_HEIGHT));
  menu_layer_set_normal_colors(s_menu_layer, theme_bg(), theme_text());
  menu_layer_set_highlight_colors(s_menu_layer, theme_purple(), theme_selected_text());
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_sections = menu_get_num_sections_callback,
    .get_num_rows = menu_get_num_rows_callback,
    .get_header_height = menu_get_header_height_callback,
    .get_cell_height = menu_get_cell_height_callback,
    .draw_header = menu_draw_header_callback,
    .draw_row = menu_draw_row_callback,
    .select_click = menu_select_callback
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

static void menu_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  text_layer_destroy(s_status_layer);
  s_menu_layer = NULL;
  s_status_layer = NULL;
}

static void detail_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  window_set_background_color(window, theme_bg());

  s_detail_header_layer = layer_create(GRect(0, 0, bounds.size.w, DETAIL_HEADER_HEIGHT));
  if (!s_detail_header_layer) {
    return;
  }
  layer_set_update_proc(s_detail_header_layer, detail_header_update_proc);
  layer_add_child(window_layer, s_detail_header_layer);

  s_scroll_layer = scroll_layer_create(GRect(0, DETAIL_HEADER_HEIGHT, bounds.size.w, bounds.size.h - DETAIL_HEADER_HEIGHT));
  if (!s_scroll_layer) {
    return;
  }
  layer_add_child(window_layer, scroll_layer_get_layer(s_scroll_layer));
  scroll_layer_set_content_size(s_scroll_layer, GSize(bounds.size.w, DETAIL_CONTENT_HEIGHT));

  s_detail_layer = layer_create(GRect(0, 0, bounds.size.w, DETAIL_CONTENT_HEIGHT));
  if (!s_detail_layer) {
    return;
  }
  layer_set_update_proc(s_detail_layer, detail_content_update_proc);
  scroll_layer_add_child(s_scroll_layer, s_detail_layer);

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
  reset_sessions();
  reset_projects();
  set_status(BUILD_LABEL " starting");

  s_menu_window = window_create();
  window_set_window_handlers(s_menu_window, (WindowHandlers) {
    .load = menu_window_load,
    .unload = menu_window_unload
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

  window_stack_push(s_menu_window, true);
  schedule_refresh(700);
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
  window_destroy(s_detail_window);
  window_destroy(s_menu_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
