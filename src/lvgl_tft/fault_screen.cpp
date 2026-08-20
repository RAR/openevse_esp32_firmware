// src/lvgl_tft/fault_screen.cpp — see fault_screen.h.
#ifdef ENABLE_SCREEN_LVGL_TFT

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

#include "fault_screen.h"
#include "nightshift.h"
#include "screen_common.h"
#include "fault_text.h"
#include "openevse.h"     // OPENEVSE_STATE_*

// fault_text.h mirrors the controller's state codes so the copy can be tested
// on the host without dragging in RapiSender and Arduino. This is where the two
// sets meet, so this is where they are checked: if the library ever renumbers a
// state, the device build stops here rather than quietly showing the wrong
// remedy for a fault.
static_assert(FAULT_STATE_VENT_REQUIRED        == OPENEVSE_STATE_VENT_REQUIRED,        "state code drift");
static_assert(FAULT_STATE_DIODE_CHECK_FAILED   == OPENEVSE_STATE_DIODE_CHECK_FAILED,   "state code drift");
static_assert(FAULT_STATE_GFI_FAULT            == OPENEVSE_STATE_GFI_FAULT,            "state code drift");
static_assert(FAULT_STATE_NO_EARTH_GROUND      == OPENEVSE_STATE_NO_EARTH_GROUND,      "state code drift");
static_assert(FAULT_STATE_STUCK_RELAY          == OPENEVSE_STATE_STUCK_RELAY,          "state code drift");
static_assert(FAULT_STATE_GFI_SELF_TEST_FAILED == OPENEVSE_STATE_GFI_SELF_TEST_FAILED, "state code drift");
static_assert(FAULT_STATE_OVER_TEMPERATURE     == OPENEVSE_STATE_OVER_TEMPERATURE,     "state code drift");
static_assert(FAULT_STATE_OVER_CURRENT         == OPENEVSE_STATE_OVER_CURRENT,         "state code drift");
static_assert(FAULT_STATE_RELAY_CLOSURE_FAULT  == OPENEVSE_STATE_RELAY_CLOSURE_FAULT,  "state code drift");
static_assert(FAULT_STATE_PP_SHORTED           == OPENEVSE_STATE_PP_SHORTED,           "state code drift");
static_assert(FAULT_STATE_PP_MISSING           == OPENEVSE_STATE_PP_MISSING,           "state code drift");
static_assert(FAULT_STATE_EEPROM_FAILURE       == OPENEVSE_STATE_EEPROM_FAILURE,       "state code drift");

#define COL_BG    NS_SURFACE
#define COL_TEXT  NS_TEXT
#define COL_DIM   NS_TEXTDIM
#define COL_FAULT NS_ERROR
#define COL_RULE  NS_BORDER

// Left margin shared by every row, so the title, the rule, the sentence and the
// steps all hang off one edge.
#define PAD_X      20
#define BODY_W    (480 - PAD_X * 2)

#define TITLE_Y    14
#define RULE_Y     60
#define WHAT_Y     72
// Three wrapped lines of "what" at montserrat_16 reach ~132; start the steps
// below that so the block never depends on how long a particular sentence is.
#define STEPS_Y   146
#define STEP_DY    28

static lv_obj_t *fault_scr = nullptr;
static lv_obj_t *title_lbl = nullptr;
static lv_obj_t *what_lbl  = nullptr;
static lv_obj_t *step_lbl[FAULT_STEPS_MAX] = { nullptr };
static lv_obj_t *ip_lbl    = nullptr;
static lv_obj_t *wifi_lbl  = nullptr;

static lv_obj_t *make_foot(lv_obj_t *parent, lv_align_t align, lv_coord_t dx)
{
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, "");
  lv_obj_set_style_text_color(l, COL_DIM, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_align(l, align, dx, -8);
  return l;
}

void fault_screen_build()
{
  fault_scr = lv_obj_create(NULL);
  lv_scr_load(fault_scr);
  lv_obj_set_style_bg_color(fault_scr, COL_BG, 0);
  lv_obj_clear_flag(fault_scr, LV_OBJ_FLAG_SCROLLABLE);

  // --- Title: warning glyph + the fault's own name ---
  title_lbl = lv_label_create(fault_scr);
  lv_label_set_text(title_lbl, "");
  lv_obj_set_style_text_color(title_lbl, COL_FAULT, 0);
  lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_28, 0);
  lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, PAD_X, TITLE_Y);

  // --- Rule, separating the name from the explanation ---
  lv_obj_t *rule = lv_obj_create(fault_scr);
  lv_obj_set_size(rule, BODY_W, 2);
  lv_obj_set_style_bg_color(rule, COL_RULE, 0);
  lv_obj_set_style_border_width(rule, 0, 0);
  lv_obj_set_style_radius(rule, 0, 0);
  lv_obj_align(rule, LV_ALIGN_TOP_LEFT, PAD_X, RULE_Y);

  // --- What it means ---
  what_lbl = lv_label_create(fault_scr);
  lv_label_set_text(what_lbl, "");
  lv_obj_set_width(what_lbl, BODY_W);
  lv_label_set_long_mode(what_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(what_lbl, COL_TEXT, 0);
  lv_obj_set_style_text_font(what_lbl, &lv_font_montserrat_16, 0);
  lv_obj_align(what_lbl, LV_ALIGN_TOP_LEFT, PAD_X, WHAT_Y);

  // --- What to do: fixed rows, so the block doesn't shift between faults ---
  for(int i = 0; i < FAULT_STEPS_MAX; i++) {
    step_lbl[i] = lv_label_create(fault_scr);
    lv_label_set_text(step_lbl[i], "");
    lv_obj_set_style_text_color(step_lbl[i], COL_DIM, 0);
    lv_obj_set_style_text_font(step_lbl[i], &lv_font_montserrat_16, 0);
    lv_obj_align(step_lbl[i], LV_ALIGN_TOP_LEFT, PAD_X, STEPS_Y + i * STEP_DY);
  }

  // --- Footer: the address is the way off this screen, so it is always here ---
  ip_lbl    = make_foot(fault_scr, LV_ALIGN_BOTTOM_LEFT,  PAD_X);
  wifi_lbl  = make_foot(fault_scr, LV_ALIGN_BOTTOM_RIGHT, -PAD_X);
}

void fault_screen_update(const FaultScreenData &d)
{
  if(!fault_scr) return;

  char buf[96];

  const FaultText *f = fault_text(d.evse_state);
  if(f) {
    snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  %s", f->title);
    lv_label_set_text(title_lbl, buf);
    lv_label_set_text(what_lbl, f->what);
    for(int i = 0; i < FAULT_STEPS_MAX; i++) {
      if(f->steps[i]) {
        snprintf(buf, sizeof(buf), "%s  %s", LV_SYMBOL_RIGHT, f->steps[i]);
        lv_label_set_text(step_lbl[i], buf);
        lv_obj_clear_flag(step_lbl[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(step_lbl[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  } else {
    // The caller only loads this screen for a fault, so this is a caller bug
    // rather than a state to render. Say so plainly instead of showing an empty
    // page that looks like the fault cleared.
    lv_label_set_text(title_lbl, LV_SYMBOL_WARNING "  FAULT");
    lv_label_set_text(what_lbl, "The charger reported a fault this firmware does not recognise.");
    for(int i = 0; i < FAULT_STEPS_MAX; i++) {
      lv_obj_add_flag(step_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Both name and address: the name is easier to type, the address still works
  // when mDNS does not. They share the footer with only the WiFi chip, so there
  // is room for the pair.
  if(d.hostname && d.hostname[0] && d.ip && d.ip[0]) {
    snprintf(buf, sizeof(buf), "%s   %s", d.hostname, d.ip);
    lv_label_set_text(ip_lbl, buf);
  } else if(d.ip && d.ip[0]) {
    lv_label_set_text(ip_lbl, d.ip);
  } else {
    lv_label_set_text(ip_lbl, (d.hostname && d.hostname[0]) ? d.hostname : "");
  }

  if(d.wifi_client) {
    if(d.wifi_connected) {
      snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %d%%", d.wifi_pct);
    } else {
      snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " --");
    }
  } else {
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " AP %d", d.sta_count);
  }
  lv_label_set_text(wifi_lbl, buf);
}

void fault_screen_destroy()
{
  if(fault_scr) {
    lv_obj_del(fault_scr);
    fault_scr = nullptr;
    title_lbl = nullptr;
    what_lbl  = nullptr;
    for(int i = 0; i < FAULT_STEPS_MAX; i++) step_lbl[i] = nullptr;
    ip_lbl    = nullptr;
    wifi_lbl  = nullptr;
  }
}

#endif // ENABLE_SCREEN_LVGL_TFT
