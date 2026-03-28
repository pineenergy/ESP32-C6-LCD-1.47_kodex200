#include "LVGL_Example.h"

/**********************
 *      TYPEDEFS
 **********************/
typedef enum
{
  DISP_SMALL,
  DISP_MEDIUM,
  DISP_LARGE,
} disp_size_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void Onboard_create(lv_obj_t *parent);
static void format_price_with_comma(int32_t price, char *out, size_t out_size);
static void format_signed_with_comma(int32_t value, char *out, size_t out_size);
static void format_signed_percent_bp(int32_t bp, char *out, size_t out_size);
static lv_color_t get_trend_color(int32_t diff);

void example1_increase_lvgl_tick(lv_timer_t *t);
/**********************
 *  STATIC VARIABLES
 **********************/
static disp_size_t disp_size;

static lv_obj_t *tv;
lv_style_t style_text_muted;
lv_style_t style_title;
lv_style_t style_value;
lv_style_t style_symbol;
static lv_style_t style_icon;
static lv_style_t style_bullet;

static const lv_font_t *font_large;
static const lv_font_t *font_normal;

static lv_timer_t *auto_step_timer;

static lv_timer_t *meter2_timer;
static char s_last_kodex200_text[100] = {0};
static char s_last_smr_text[100] = {0};
static uint32_t s_last_update_sequence = 0;
static int32_t s_refresh_remaining_sec = CONFIG_APP_STOCK_UPDATE_PERIOD_SEC;

static lv_obj_t *kodex200_title_label;
static lv_obj_t *kodexsmr_title_label;
static lv_obj_t *refresh_bar;

lv_obj_t *SD_Size;
lv_obj_t *FlashSize;
lv_obj_t *Board_angle;
lv_obj_t *RTC_Time;
lv_obj_t *KODEX200_Field;
lv_obj_t *KODEXSMR_Field;

void IRAM_ATTR auto_switch(lv_timer_t *t)
{
  uint16_t page = lv_tabview_get_tab_act(tv);

  if (page == 0)
  {
    lv_tabview_set_act(tv, 1, LV_ANIM_ON);
  }
  else if (page == 3)
  {
    lv_tabview_set_act(tv, 2, LV_ANIM_ON);
  }
}
void Lvgl_Example1(void)
{

  disp_size = DISP_SMALL;

  font_large = LV_FONT_DEFAULT;
  font_normal = LV_FONT_DEFAULT;

#if LV_FONT_MONTSERRAT_18
  font_large = &lv_font_montserrat_18;
#else
  LV_LOG_WARN("LV_FONT_MONTSERRAT_18 is not enabled for the widgets demo. Using LV_FONT_DEFAULT instead.");
#endif
#if LV_FONT_MONTSERRAT_12
  font_normal = &lv_font_montserrat_12;
#else
  LV_LOG_WARN("LV_FONT_MONTSERRAT_12 is not enabled for the widgets demo. Using LV_FONT_DEFAULT instead.");
#endif
#if LV_FONT_MONTSERRAT_16
  font_normal = &lv_font_montserrat_16;
#endif

  lv_style_init(&style_text_muted);
  lv_style_set_text_opa(&style_text_muted, LV_OPA_90);

  lv_style_init(&style_title);
  lv_style_set_text_font(&style_title, font_large);

  lv_style_init(&style_value);
  lv_style_set_text_font(&style_value, font_large);

  lv_style_init(&style_symbol);
  lv_style_set_text_font(&style_symbol, font_large);
  lv_style_set_text_color(&style_symbol, lv_color_hex(0x000000));

  lv_style_init(&style_icon);
  lv_style_set_text_color(&style_icon, lv_theme_get_color_primary(NULL));
  lv_style_set_text_font(&style_icon, font_large);

  lv_style_init(&style_bullet);
  lv_style_set_border_width(&style_bullet, 0);
  lv_style_set_radius(&style_bullet, LV_RADIUS_CIRCLE);

  lv_obj_set_style_text_font(lv_scr_act(), font_normal, 0);

  Onboard_create(lv_scr_act());
}

void Lvgl_Example1_close(void)
{
  /*Delete all animation*/
  lv_anim_del(NULL, NULL);

  lv_timer_del(meter2_timer);
  meter2_timer = NULL;

  lv_obj_clean(lv_scr_act());

  lv_style_reset(&style_text_muted);
  lv_style_reset(&style_title);
  lv_style_reset(&style_value);
  lv_style_reset(&style_symbol);
  lv_style_reset(&style_icon);
  lv_style_reset(&style_bullet);
}

static void format_signed_with_comma(int32_t value, char *out, size_t out_size)
{
  char numbuf[24] = {0};
  int32_t abs_value = value < 0 ? -value : value;
  format_price_with_comma(abs_value, numbuf, sizeof(numbuf));

  if (value > 0)
  {
    snprintf(out, out_size, "+%s", numbuf);
  }
  else if (value < 0)
  {
    snprintf(out, out_size, "-%s", numbuf);
  }
  else
  {
    snprintf(out, out_size, "0");
  }
}

static void format_signed_percent_bp(int32_t bp, char *out, size_t out_size)
{
  char sign = '+';
  int32_t abs_bp = bp;
  if (bp < 0)
  {
    sign = '-';
    abs_bp = -bp;
  }

  int32_t int_part = abs_bp / 100;
  int32_t frac_part = abs_bp % 100;
  snprintf(out, out_size, "%c%ld.%02ld", sign, (long)int_part, (long)frac_part);
}

static void format_price_with_comma(int32_t price, char *out, size_t out_size)
{
  char digits[24] = {0};
  bool negative = price < 0;
  long abs_value = negative ? -(long)price : (long)price;
  snprintf(digits, sizeof(digits), "%ld", abs_value);

  int len = strlen(digits);
  int comma_count = (len > 0) ? (len - 1) / 3 : 0;
  int total_len = len + comma_count + (negative ? 1 : 0);
  if ((size_t)(total_len + 1) > out_size)
  {
    snprintf(out, out_size, "%ld", (long)price);
    return;
  }

  out[total_len] = '\0';
  int src = len - 1;
  int dst = total_len - 1;
  int group = 0;

  while (src >= 0)
  {
    out[dst--] = digits[src--];
    group++;
    if (group == 3 && src >= 0)
    {
      out[dst--] = ',';
      group = 0;
    }
  }

  if (negative)
  {
    out[0] = '-';
  }
}

static lv_color_t get_trend_color(int32_t diff)
{
  if (diff > 0)
  {
    return lv_color_hex(0xD00000);
  }
  if (diff < 0)
  {
    return lv_color_hex(0x0050D0);
  }
  return lv_color_hex(0x505050);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void Onboard_create(lv_obj_t *parent)
{
  lv_obj_t *panel1 = lv_obj_create(parent);
  lv_obj_set_size(panel1, lv_pct(100), lv_pct(100));
  lv_obj_set_style_pad_all(panel1, 14, 0);
  lv_obj_set_style_pad_row(panel1, 14, 0);
  lv_obj_set_flex_flow(panel1, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(panel1, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(panel1, LV_SCROLLBAR_MODE_OFF);

  kodex200_title_label = lv_label_create(panel1);
  lv_label_set_text(kodex200_title_label, "KODEX200");
  lv_obj_add_style(kodex200_title_label, &style_symbol, 0);

  SD_Size = lv_label_create(panel1);
  lv_obj_set_width(SD_Size, lv_pct(100));
  lv_label_set_long_mode(SD_Size, LV_LABEL_LONG_CLIP);
  lv_label_set_text(SD_Size, "Loading...");
  lv_obj_add_style(SD_Size, &style_value, 0);

  kodexsmr_title_label = lv_label_create(panel1);
  lv_label_set_text(kodexsmr_title_label, "KODEX SMR");
  lv_obj_add_style(kodexsmr_title_label, &style_symbol, 0);

  FlashSize = lv_label_create(panel1);
  lv_obj_set_width(FlashSize, lv_pct(100));
  lv_label_set_long_mode(FlashSize, LV_LABEL_LONG_CLIP);
  lv_label_set_text(FlashSize, "Loading...");
  lv_obj_add_style(FlashSize, &style_value, 0);

  refresh_bar = lv_bar_create(panel1);
  lv_obj_set_width(refresh_bar, lv_pct(100));
  lv_obj_set_height(refresh_bar, 6);
  lv_bar_set_range(refresh_bar, 0, CONFIG_APP_STOCK_UPDATE_PERIOD_SEC);
  lv_bar_set_value(refresh_bar, CONFIG_APP_STOCK_UPDATE_PERIOD_SEC, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(refresh_bar, lv_color_hex(0xD7D9DC), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(refresh_bar, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_bg_color(refresh_bar, lv_color_hex(0x505050), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(refresh_bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(refresh_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_PART_INDICATOR);

  KODEX200_Field = SD_Size;
  KODEXSMR_Field = FlashSize;

  auto_step_timer = lv_timer_create(example1_increase_lvgl_tick, 1000, NULL);
}

void example1_increase_lvgl_tick(lv_timer_t *t)
{
  char buf[100] = {0};
  char numbuf[32] = {0};
  char diffbuf[32] = {0};
  char pctbuf[24] = {0};
  int32_t diff = 0;
  lv_color_t flat_color = lv_color_hex(0x202020);

  if (ETF_Update_Sequence != s_last_update_sequence)
  {
    s_last_update_sequence = ETF_Update_Sequence;
    s_refresh_remaining_sec = CONFIG_APP_STOCK_UPDATE_PERIOD_SEC;
  }
  else if (s_refresh_remaining_sec > 0)
  {
    s_refresh_remaining_sec--;
  }

  lv_bar_set_value(refresh_bar, s_refresh_remaining_sec, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(refresh_bar, get_trend_color(KODEX200_Change), LV_PART_INDICATOR);

  if (ETF_Price_Valid)
  {
    format_price_with_comma(KODEX200_Price, numbuf, sizeof(numbuf));
    diff = KODEX200_Change;
    format_signed_with_comma(diff, diffbuf, sizeof(diffbuf));
    format_signed_percent_bp(KODEX200_ChangeBp, pctbuf, sizeof(pctbuf));
    snprintf(buf, sizeof(buf), "%s KRW (%s, %s%%)", numbuf, diffbuf, pctbuf);
    if (strcmp(s_last_kodex200_text, buf) != 0)
    {
      lv_label_set_text(SD_Size, buf);
      strncpy(s_last_kodex200_text, buf, sizeof(s_last_kodex200_text) - 1);
    }
    lv_obj_set_style_text_color(SD_Size, get_trend_color(diff), 0);

    format_price_with_comma(KODEXSMR_Price, numbuf, sizeof(numbuf));
    diff = KODEXSMR_Change;
    format_signed_with_comma(diff, diffbuf, sizeof(diffbuf));
    format_signed_percent_bp(KODEXSMR_ChangeBp, pctbuf, sizeof(pctbuf));
    snprintf(buf, sizeof(buf), "%s KRW (%s, %s%%)", numbuf, diffbuf, pctbuf);
    if (strcmp(s_last_smr_text, buf) != 0)
    {
      lv_label_set_text(FlashSize, buf);
      strncpy(s_last_smr_text, buf, sizeof(s_last_smr_text) - 1);
    }
    lv_obj_set_style_text_color(FlashSize, get_trend_color(diff), 0);
  }
  else
  {
    snprintf(buf, sizeof(buf), "%.95s", ETF_Status);
    if (strcmp(s_last_kodex200_text, buf) != 0)
    {
      lv_label_set_text(SD_Size, buf);
      strncpy(s_last_kodex200_text, buf, sizeof(s_last_kodex200_text) - 1);
    }
    lv_obj_set_style_text_color(SD_Size, flat_color, 0);

    snprintf(buf, sizeof(buf), "%.95s", ETF_Status);
    if (strcmp(s_last_smr_text, buf) != 0)
    {
      lv_label_set_text(FlashSize, buf);
      strncpy(s_last_smr_text, buf, sizeof(s_last_smr_text) - 1);
    }
    lv_obj_set_style_text_color(FlashSize, flat_color, 0);
  }
}
