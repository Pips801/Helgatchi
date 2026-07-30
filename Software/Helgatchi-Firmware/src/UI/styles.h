#ifndef EEZ_LVGL_UI_STYLES_H
#define EEZ_LVGL_UI_STYLES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Style: Focused - Dropdown
lv_style_t *get_style_focused___dropdown_MAIN_DEFAULT();
lv_style_t *get_style_focused___dropdown_MAIN_FOCUS_KEY();
void add_style_focused___dropdown(lv_obj_t *obj);
void remove_style_focused___dropdown(lv_obj_t *obj);

// Style: Focused - Switch
lv_style_t *get_style_focused___switch_MAIN_DEFAULT();
lv_style_t *get_style_focused___switch_MAIN_FOCUS_KEY();
lv_style_t *get_style_focused___switch_INDICATOR_CHECKED();
void add_style_focused___switch(lv_obj_t *obj);
void remove_style_focused___switch(lv_obj_t *obj);

// Style: Default Main Content
lv_style_t *get_style_default_main_content_MAIN_DEFAULT();
void add_style_default_main_content(lv_obj_t *obj);
void remove_style_default_main_content(lv_obj_t *obj);

// Style: Default Main Menu Panel
lv_style_t *get_style_default_main_menu_panel_MAIN_DEFAULT();
lv_style_t *get_style_default_main_menu_panel_MAIN_FOCUS_KEY();
void add_style_default_main_menu_panel(lv_obj_t *obj);
void remove_style_default_main_menu_panel(lv_obj_t *obj);

// Style: Default Main Content (Panel)
lv_style_t *get_style_default_main_content__panel__MAIN_DEFAULT();
void add_style_default_main_content__panel_(lv_obj_t *obj);
void remove_style_default_main_content__panel_(lv_obj_t *obj);

// Style: Alert Card
lv_style_t *get_style_alert_card_MAIN_DEFAULT();
lv_style_t *get_style_alert_card_MAIN_FOCUS_KEY();
void add_style_alert_card(lv_obj_t *obj);
void remove_style_alert_card(lv_obj_t *obj);

// Style: Focused - Button
lv_style_t *get_style_focused___button_MAIN_DEFAULT();
lv_style_t *get_style_focused___button_MAIN_FOCUS_KEY();
void add_style_focused___button(lv_obj_t *obj);
void remove_style_focused___button(lv_obj_t *obj);

// Style: Device Card
lv_style_t *get_style_device_card_MAIN_DEFAULT();
lv_style_t *get_style_device_card_MAIN_FOCUS_KEY();
void add_style_device_card(lv_obj_t *obj);
void remove_style_device_card(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_STYLES_H*/