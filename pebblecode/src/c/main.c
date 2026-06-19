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
#define MAX_TEXT 1536
#define REFRESH_INTERVAL_MS 300000
#define BUILD_LABEL "v0.1.0"
#define DETAIL_CONTENT_HEIGHT 1200
#define STATUS_BAR_HEIGHT 20
#define DETAIL_HEADER_HEIGHT 50
#define PANEL_EDGE 5

typedef struct {
  char id[72];
  char title[64];
  char directory[48];
  char agent[24];
  char status[18];
  char summary[220];
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

static Window *s_menu_window;
static Window *s_detail_window;
static MenuLayer *s_menu_layer;
static TextLayer *s_status_layer;
static ScrollLayer *s_scroll_layer;
static Layer *s_detail_header_layer;
static TextLayer *s_detail_layer;
static DictationSession *s_dictation;
static AppTimer *s_refresh_timer;

static SessionItem s_sessions[MAX_SESSIONS];
static ProjectItem s_projects[MAX_PROJECTS];
static int s_session_count;
static int s_project_count;
static int s_expected_count;
static int s_selected_index = -1;
static int s_selected_project_index = -1;
static bool s_loading;
static bool s_initial_sync = true;
static bool s_showing_context;
static DictationTarget s_dictation_target;
static char s_status_text[64];
static char s_detail_text[MAX_TEXT];
static char s_full_context[MAX_TEXT];

static void request_refresh(void);
static void request_detail(int index, bool full_context);
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

static void update_detail_text(void) {
  if (s_selected_index < 0 || s_selected_index >= s_session_count) {
    return;
  }

  SessionItem *item = &s_sessions[s_selected_index];
  const char *summary = item->summary[0] ? item->summary : "No assistant summary yet.";
  const char *context = s_showing_context && s_full_context[0] ? s_full_context : "";

  if (s_showing_context) {
    char context_preview[1200];
    strncpy(context_preview, context[0] ? context : "Loading full context...", sizeof(context_preview) - 1);
    context_preview[sizeof(context_preview) - 1] = '\0';
    snprintf(
      s_detail_text,
      sizeof(s_detail_text),
      "[AGENT] %s\n[PATH]  %s\n\n> SUMMARY\n%s\n\n> FULL CONTEXT\n%s\n\n[SELECT] full context\n[HOLD]   voice reply\n[BACK]   dashboard",
      item->agent[0] ? item->agent : "agent",
      item->directory[0] ? short_path(item->directory) : "-",
      summary,
      context_preview
    );
  } else {
    snprintf(
      s_detail_text,
      sizeof(s_detail_text),
      "[AGENT] %s\n[PATH]  %s\n\n> SUMMARY\n%s\n\n[SELECT]      full context\n[HOLD SELECT] voice reply\n[BACK]        dashboard",
      item->agent[0] ? item->agent : "agent",
      item->directory[0] ? short_path(item->directory) : "-",
      summary
    );
  }

  if (s_detail_layer && s_scroll_layer) {
    text_layer_set_text(s_detail_layer, s_detail_text);
    GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
    int width = scroll_bounds.size.w;
    int height = DETAIL_CONTENT_HEIGHT;
    if (height < scroll_bounds.size.h) {
      height = scroll_bounds.size.h;
    }
    text_layer_set_size(s_detail_layer, GSize(width - 12, height));
    scroll_layer_set_content_size(s_scroll_layer, GSize(width, height));
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
  app_message_outbox_send();
}

static void request_detail(int index, bool full_context) {
  if (index < 0 || index >= s_session_count) {
    return;
  }

  s_selected_index = index;
  s_showing_context = full_context;
  if (full_context) {
    s_full_context[0] = '\0';
    set_status("Loading context...");
  } else {
    set_status("Loading detail...");
  }
  update_detail_text();

  DictionaryIterator *iter = NULL;
  send_command_begin(&iter, full_context ? CMD_CONTEXT : CMD_DETAIL);
  if (!iter) {
    return;
  }
  dict_write_int(iter, KEY_INDEX, &index, sizeof(index), true);
  dict_write_cstring(iter, KEY_SESSION_ID, s_sessions[index].id);
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
  set_status("Sending reply...");
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
  set_status("Starting thread...");
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
  return 42;
}

static void menu_draw_header_callback(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *data) {
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_fill_color(ctx, theme_deep_purple());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_bg());
  graphics_fill_rect(ctx, GRect(PANEL_EDGE, 3, bounds.size.w - (PANEL_EDGE * 2), bounds.size.h - 6), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_purple());
  graphics_fill_rect(ctx, GRect(0, 0, PANEL_EDGE, bounds.size.h), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_green());
  graphics_fill_rect(ctx, GRect(PANEL_EDGE, bounds.size.h - 2, bounds.size.w - PANEL_EDGE, 2), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_orange());
  graphics_fill_rect(ctx, GRect(bounds.size.w - 18, 0, 18, 3), 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, theme_bright_purple());
  graphics_draw_line(ctx, GPoint(PANEL_EDGE, 2), GPoint(bounds.size.w - 20, 2));
  graphics_context_set_text_color(ctx, theme_green());
  graphics_draw_text(ctx, section_index == 1 ? "NEW THREAD" : "T3CODE AGENTS", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
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
  return GColorFromHEX(0x05030A);
#else
  return GColorBlack;
#endif
}

static GColor theme_panel(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x180B26);
#else
  return GColorBlack;
#endif
}

static GColor theme_purple(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x5517A8);
#else
  return GColorBlack;
#endif
}

static GColor theme_deep_purple(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x10071A);
#else
  return GColorBlack;
#endif
}

static GColor theme_bright_purple(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x7C35FF);
#else
  return GColorBlack;
#endif
}

static GColor theme_green(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xA6FF00);
#else
  return GColorWhite;
#endif
}

static GColor theme_orange(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xFF6500);
#else
  return GColorWhite;
#endif
}

static GColor theme_muted(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xA89BB8);
#else
  return GColorWhite;
#endif
}

static GColor theme_text(void) {
  return GColorWhite;
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
    return GColorFromHEX(0x14C85A);
  }
  if (strcmp(status, "Error") == 0) {
    return GColorRed;
  }
#endif
  return GColorWhite;
}

static void menu_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
  GRect bounds = layer_get_bounds(cell_layer);

  if (cell_index->section == 1) {
    ProjectItem *project = &s_projects[cell_index->row];
    char title[88];
    char subtitle[88];
    snprintf(title, sizeof(title), "+ %s", project->title[0] ? project->title : "New thread");
    snprintf(subtitle, sizeof(subtitle), "%s | %s",
             project->model[0] ? project->model : "codex",
             project->directory[0] ? short_path(project->directory) : "-");

    bool selected = menu_layer_is_index_selected(s_menu_layer, cell_index);
    graphics_context_set_fill_color(ctx, theme_bg());
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    GRect panel = GRect(6, 3, bounds.size.w - 10, bounds.size.h - 5);
    graphics_context_set_fill_color(ctx, selected ? theme_purple() : (cell_index->row % 2 ? theme_panel() : theme_deep_purple()));
    graphics_fill_rect(ctx, panel, 0, GCornerNone);

    if (!selected) {
      graphics_context_set_fill_color(ctx, theme_bg());
      graphics_fill_rect(ctx, GRect(panel.origin.x + 4, panel.origin.y + 3, panel.size.w - 8, panel.size.h - 6), 0, GCornerNone);
    }

    graphics_context_set_fill_color(ctx, theme_green());
    graphics_fill_rect(ctx, GRect(0, 0, PANEL_EDGE, bounds.size.h), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, selected ? theme_green() : theme_deep_purple());
    graphics_fill_rect(ctx, GRect(9, 5, 13, 13), 0, GCornerNone);
    graphics_context_set_text_color(ctx, selected ? GColorBlack : theme_green());
    graphics_draw_text(ctx, "+", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(9, 2, 13, 16), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    if (selected) {
      graphics_context_set_fill_color(ctx, theme_green());
      graphics_fill_rect(ctx, GRect(PANEL_EDGE, 0, bounds.size.w - PANEL_EDGE, 2), 0, GCornerNone);
      graphics_context_set_fill_color(ctx, theme_bright_purple());
      graphics_fill_rect(ctx, GRect(23, 4, 3, bounds.size.h - 8), 0, GCornerNone);
    } else {
      graphics_context_set_stroke_color(ctx, theme_purple());
      graphics_draw_line(ctx, GPoint(6, 3), GPoint(bounds.size.w - 16, 3));
    }

    graphics_context_set_text_color(ctx, selected ? theme_green() : theme_text());
    graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(27, -1, bounds.size.w - 36, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_context_set_text_color(ctx, theme_muted());
    graphics_draw_text(ctx, subtitle, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(27, 21, bounds.size.w - 36, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    graphics_context_set_stroke_color(ctx, selected ? theme_green() : theme_deep_purple());
    graphics_draw_line(ctx, GPoint(PANEL_EDGE, bounds.size.h - 1), GPoint(bounds.size.w, bounds.size.h - 1));
    return;
  }

  if (showing_initial_sync() || s_session_count == 0) {
    graphics_context_set_fill_color(ctx, theme_bg());
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_fill_color(ctx, theme_panel());
    graphics_fill_rect(ctx, GRect(6, 4, bounds.size.w - 12, bounds.size.h - 7), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, theme_purple());
    graphics_fill_rect(ctx, GRect(0, 0, PANEL_EDGE, bounds.size.h), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, theme_green());
    graphics_fill_rect(ctx, GRect(PANEL_EDGE, 0, bounds.size.w - PANEL_EDGE, 2), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, theme_orange());
    graphics_fill_rect(ctx, GRect(bounds.size.w - 6, 4, 3, bounds.size.h - 9), 0, GCornerNone);
    graphics_context_set_text_color(ctx, theme_green());
    graphics_draw_text(ctx, showing_initial_sync() || s_loading ? "SYNCING AGENTS" : "NO AGENTS", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(10, -1, bounds.size.w - 14, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_context_set_text_color(ctx, theme_muted());
    graphics_draw_text(ctx, showing_initial_sync() ? "Opening T3 Code link" : (s_loading ? "Loading full agent list" : "Press Select to refresh"), fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(10, 21, bounds.size.w - 14, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
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
  graphics_context_set_fill_color(ctx, theme_bg());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  GRect panel = GRect(6, 3, bounds.size.w - 10, bounds.size.h - 5);
  graphics_context_set_fill_color(ctx, selected ? theme_purple() : (cell_index->row % 2 ? theme_panel() : theme_deep_purple()));
  graphics_fill_rect(ctx, panel, 0, GCornerNone);

  if (!selected) {
    graphics_context_set_fill_color(ctx, theme_bg());
    graphics_fill_rect(ctx, GRect(panel.origin.x + 4, panel.origin.y + 3, panel.size.w - 8, panel.size.h - 6), 0, GCornerNone);
  }

  graphics_context_set_fill_color(ctx, status_color(item->status));
  graphics_fill_rect(ctx, GRect(0, 0, PANEL_EDGE, bounds.size.h), 0, GCornerNone);

  graphics_context_set_fill_color(ctx, selected ? theme_green() : theme_deep_purple());
  graphics_fill_rect(ctx, GRect(9, 5, 13, 13), 0, GCornerNone);
  graphics_context_set_text_color(ctx, selected ? GColorBlack : status_color(item->status));
  graphics_draw_text(ctx, status_prefix(item->status), fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(9, 2, 13, 16), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  if (strcmp(item->status, "Needs input") == 0 || strcmp(item->status, "Error") == 0) {
    graphics_context_set_fill_color(ctx, theme_orange());
    graphics_fill_rect(ctx, GRect(bounds.size.w - 5, 3, 4, bounds.size.h - 6), 0, GCornerNone);
  }

  if (selected) {
    graphics_context_set_fill_color(ctx, theme_green());
    graphics_fill_rect(ctx, GRect(PANEL_EDGE, 0, bounds.size.w - PANEL_EDGE, 2), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, theme_bright_purple());
    graphics_fill_rect(ctx, GRect(23, 4, 3, bounds.size.h - 8), 0, GCornerNone);
  } else {
    graphics_context_set_stroke_color(ctx, theme_purple());
    graphics_draw_line(ctx, GPoint(6, 3), GPoint(bounds.size.w - 16, 3));
  }

  graphics_context_set_text_color(ctx, selected ? theme_green() : theme_text());
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(27, -1, bounds.size.w - 36, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, strcmp(item->status, "Needs input") == 0 ? theme_orange() : theme_muted());
  graphics_draw_text(ctx, subtitle, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(27, 21, bounds.size.w - 36, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  graphics_context_set_stroke_color(ctx, selected ? theme_green() : theme_deep_purple());
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
  graphics_context_set_fill_color(ctx, status_color(item->status));
  graphics_fill_rect(ctx, GRect(0, 0, PANEL_EDGE, bounds.size.h), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_purple());
  graphics_fill_rect(ctx, GRect(PANEL_EDGE, 0, bounds.size.w - PANEL_EDGE, 3), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_green());
  graphics_fill_rect(ctx, GRect(PANEL_EDGE, bounds.size.h - 2, bounds.size.w - PANEL_EDGE, 2), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, theme_bright_purple());
  graphics_fill_rect(ctx, GRect(9, 6, 3, bounds.size.h - 14), 0, GCornerNone);

  if (strcmp(item->status, "Needs input") == 0 || strcmp(item->status, "Error") == 0) {
    graphics_context_set_fill_color(ctx, theme_orange());
    graphics_fill_rect(ctx, GRect(bounds.size.w - 20, 0, 20, 3), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(bounds.size.w - 5, 6, 4, bounds.size.h - 14), 0, GCornerNone);
  }

  graphics_context_set_text_color(ctx, theme_text());
  graphics_draw_text(ctx, item->title[0] ? item->title : "Untitled",
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(16, 1, bounds.size.w - 26, 24),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  char status_line[64];
  snprintf(status_line, sizeof(status_line), "%s / %s",
           item->status[0] ? item->status : "-",
           s_showing_context ? "CONTEXT" : "SUMMARY");
  graphics_context_set_text_color(ctx, strcmp(item->status, "Needs input") == 0 ? theme_orange() : theme_green());
  graphics_draw_text(ctx, status_line, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(16, 25, bounds.size.w - 26, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  if (cell_index->section == 1) {
    if (cell_index->row >= s_project_count) {
      return;
    }
    s_selected_project_index = cell_index->row;
    s_dictation_target = DictationTargetProject;
    set_status("Dictate first prompt");
    start_dictation();
    return;
  }

  if (showing_initial_sync() || s_session_count == 0) {
    request_refresh();
    return;
  }
  s_selected_index = cell_index->row;
  s_showing_context = false;
  s_full_context[0] = '\0';
  window_stack_push(s_detail_window, true);
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
  request_detail(s_selected_index, true);
}

static void detail_select_long_handler(ClickRecognizerRef recognizer, void *context) {
  s_dictation_target = DictationTargetSession;
  start_dictation();
}

static void detail_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_scroll_layer) {
    scroll_layer_scroll_up_click_handler(recognizer, s_scroll_layer);
  }
}

static void detail_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_scroll_layer) {
    scroll_layer_scroll_down_click_handler(recognizer, s_scroll_layer);
  }
}

static void detail_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, detail_select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, detail_select_long_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, detail_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, detail_down_click_handler);
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
    if (!showing_initial_sync()) {
      menu_layer_reload_data(s_menu_layer);
    }
  } else if (command == CMD_SESSION_END) {
    s_loading = false;
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
    menu_layer_reload_data(s_menu_layer);
  } else if (command == CMD_PROJECT_END) {
    int total = int_tuple(iter, KEY_TOTAL, s_project_count);
    if (total == 0) {
      reset_projects();
    }
    menu_layer_reload_data(s_menu_layer);
  } else if (command == CMD_DETAIL) {
    int index = int_tuple(iter, KEY_INDEX, s_selected_index);
    if (index >= 0 && index < s_session_count) {
      s_selected_index = index;
      copy_tuple(s_sessions[index].summary, sizeof(s_sessions[index].summary), iter, KEY_SUMMARY);
      copy_tuple(s_sessions[index].status, sizeof(s_sessions[index].status), iter, KEY_STATUS);
      copy_tuple(s_sessions[index].request_id, sizeof(s_sessions[index].request_id), iter, KEY_REQUEST_ID);
      copy_tuple(s_sessions[index].request_kind, sizeof(s_sessions[index].request_kind), iter, KEY_REQUEST_KIND);
      s_showing_context = false;
      update_detail_text();
      menu_layer_reload_data(s_menu_layer);
    }
  } else if (command == CMD_CONTEXT) {
    copy_tuple(s_full_context, sizeof(s_full_context), iter, KEY_CONTEXT);
    s_showing_context = true;
    update_detail_text();
  } else if (command == CMD_PROMPT) {
    set_status("Reply sent");
    request_refresh();
  } else if (command == CMD_ERROR) {
    char error[120] = "Phone bridge error";
    copy_tuple(error, sizeof(error), iter, KEY_ERROR);
    set_status(error);
    s_loading = false;
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
  set_status("Message dropped");
}

static void outbox_failed_callback(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  s_loading = false;
  s_initial_sync = false;
  set_status("Phone send failed");
  schedule_refresh(REFRESH_INTERVAL_MS);
}

static void menu_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  window_set_background_color(window, theme_bg());

  s_status_layer = text_layer_create(GRect(0, 0, bounds.size.w, STATUS_BAR_HEIGHT));
  text_layer_set_background_color(s_status_layer, theme_deep_purple());
  text_layer_set_text_color(s_status_layer, theme_green());
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_text(s_status_layer, s_status_text);
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  s_menu_layer = menu_layer_create(GRect(0, STATUS_BAR_HEIGHT, bounds.size.w, bounds.size.h - STATUS_BAR_HEIGHT));
  menu_layer_set_normal_colors(s_menu_layer, theme_bg(), theme_text());
  menu_layer_set_highlight_colors(s_menu_layer, theme_purple(), theme_green());
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

  s_detail_layer = text_layer_create(GRect(6, 2, bounds.size.w - 12, DETAIL_CONTENT_HEIGHT));
  if (!s_detail_layer) {
    return;
  }
  text_layer_set_background_color(s_detail_layer, theme_bg());
  text_layer_set_text_color(s_detail_layer, theme_text());
  text_layer_set_font(s_detail_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_detail_layer));

  window_set_click_config_provider(window, detail_click_config_provider);
  update_detail_text();
}

static void detail_window_unload(Window *window) {
  if (s_detail_layer) {
    text_layer_destroy(s_detail_layer);
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
  app_message_open(2048, 2048);

  window_stack_push(s_menu_window, true);
  schedule_refresh(700);
}

static void deinit(void) {
  if (s_refresh_timer) {
    app_timer_cancel(s_refresh_timer);
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
