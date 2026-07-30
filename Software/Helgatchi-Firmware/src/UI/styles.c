#include "styles.h"
#include "images.h"
#include "fonts.h"

#include "ui.h"
#include "screens.h"

//
// Style: Focused - Dropdown
//

void init_style_focused___dropdown_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_align(style, LV_ALIGN_RIGHT_MID);
};

lv_style_t *get_style_focused___dropdown_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_focused___dropdown_MAIN_DEFAULT(style);
    }
    return style;
};

void init_style_focused___dropdown_MAIN_FOCUS_KEY(lv_style_t *style) {
    lv_style_set_outline_pad(style, 2);
    lv_style_set_outline_width(style, 0);
    lv_style_set_border_color(style, lv_color_hex(theme_colors[eez_flow_get_selected_theme_index()][0]));
    lv_style_set_border_width(style, 2);
};

lv_style_t *get_style_focused___dropdown_MAIN_FOCUS_KEY() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_focused___dropdown_MAIN_FOCUS_KEY(style);
    }
    return style;
};

void add_style_focused___dropdown(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_focused___dropdown_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, get_style_focused___dropdown_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
};

void remove_style_focused___dropdown(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_focused___dropdown_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, get_style_focused___dropdown_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
};

//
// Style: Focused - Switch
//

void init_style_focused___switch_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_align(style, LV_ALIGN_RIGHT_MID);
};

lv_style_t *get_style_focused___switch_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_focused___switch_MAIN_DEFAULT(style);
    }
    return style;
};

void init_style_focused___switch_MAIN_FOCUS_KEY(lv_style_t *style) {
    lv_style_set_border_color(style, lv_color_hex(theme_colors[eez_flow_get_selected_theme_index()][0]));
    lv_style_set_border_width(style, 2);
    lv_style_set_outline_width(style, 0);
    lv_style_set_border_side(style, LV_BORDER_SIDE_FULL);
};

lv_style_t *get_style_focused___switch_MAIN_FOCUS_KEY() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_focused___switch_MAIN_FOCUS_KEY(style);
    }
    return style;
};

void init_style_focused___switch_INDICATOR_CHECKED(lv_style_t *style) {
    lv_style_set_bg_color(style, lv_color_hex(theme_colors[eez_flow_get_selected_theme_index()][1]));
};

lv_style_t *get_style_focused___switch_INDICATOR_CHECKED() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_focused___switch_INDICATOR_CHECKED(style);
    }
    return style;
};

void add_style_focused___switch(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_focused___switch_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, get_style_focused___switch_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(obj, get_style_focused___switch_INDICATOR_CHECKED(), LV_PART_INDICATOR | LV_STATE_CHECKED);
};

void remove_style_focused___switch(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_focused___switch_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, get_style_focused___switch_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_remove_style(obj, get_style_focused___switch_INDICATOR_CHECKED(), LV_PART_INDICATOR | LV_STATE_CHECKED);
};

//
// Style: Default Main Content
//

void init_style_default_main_content_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_align(style, LV_ALIGN_BOTTOM_MID);
};

lv_style_t *get_style_default_main_content_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_default_main_content_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_default_main_content(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_default_main_content_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_default_main_content(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_default_main_content_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: Default Main Menu Panel
//

void init_style_default_main_menu_panel_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_layout(style, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(style, LV_FLEX_FLOW_COLUMN);
    lv_style_set_flex_main_place(style, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_cross_place(style, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(style, LV_FLEX_ALIGN_CENTER);
    lv_style_set_radius(style, 43);
};

lv_style_t *get_style_default_main_menu_panel_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_default_main_menu_panel_MAIN_DEFAULT(style);
    }
    return style;
};

void init_style_default_main_menu_panel_MAIN_FOCUS_KEY(lv_style_t *style) {
    lv_style_set_border_color(style, lv_color_hex(theme_colors[eez_flow_get_selected_theme_index()][0]));
};

lv_style_t *get_style_default_main_menu_panel_MAIN_FOCUS_KEY() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_default_main_menu_panel_MAIN_FOCUS_KEY(style);
    }
    return style;
};

void add_style_default_main_menu_panel(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_default_main_menu_panel_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, get_style_default_main_menu_panel_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
};

void remove_style_default_main_menu_panel(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_default_main_menu_panel_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, get_style_default_main_menu_panel_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
};

//
// Style: Default Main Content (Panel)
//

void init_style_default_main_content__panel__MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_radius(style, 20);
    lv_style_set_layout(style, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(style, LV_FLEX_FLOW_ROW);
    lv_style_set_align(style, LV_ALIGN_BOTTOM_MID);
};

lv_style_t *get_style_default_main_content__panel__MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_default_main_content__panel__MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_default_main_content__panel_(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_default_main_content__panel__MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_default_main_content__panel_(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_default_main_content__panel__MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: Alert Card
//

void init_style_alert_card_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_radius(style, 44);
};

lv_style_t *get_style_alert_card_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_alert_card_MAIN_DEFAULT(style);
    }
    return style;
};

void init_style_alert_card_MAIN_FOCUS_KEY(lv_style_t *style) {
    lv_style_set_border_color(style, lv_color_hex(theme_colors[eez_flow_get_selected_theme_index()][0]));
};

lv_style_t *get_style_alert_card_MAIN_FOCUS_KEY() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_alert_card_MAIN_FOCUS_KEY(style);
    }
    return style;
};

void add_style_alert_card(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_alert_card_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, get_style_alert_card_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
};

void remove_style_alert_card(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_alert_card_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, get_style_alert_card_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
};

//
// Style: Focused - Button
//

void init_style_focused___button_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_align(style, LV_ALIGN_RIGHT_MID);
    lv_style_set_radius(style, 43);
    lv_style_set_border_width(style, 2);
    lv_style_set_border_opa(style, 0);
};

lv_style_t *get_style_focused___button_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_focused___button_MAIN_DEFAULT(style);
    }
    return style;
};

void init_style_focused___button_MAIN_FOCUS_KEY(lv_style_t *style) {
    lv_style_set_border_color(style, lv_color_hex(theme_colors[eez_flow_get_selected_theme_index()][0]));
    lv_style_set_border_opa(style, 255);
    lv_style_set_border_width(style, 2);
    lv_style_set_outline_width(style, 0);
};

lv_style_t *get_style_focused___button_MAIN_FOCUS_KEY() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_focused___button_MAIN_FOCUS_KEY(style);
    }
    return style;
};

void add_style_focused___button(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_focused___button_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, get_style_focused___button_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
};

void remove_style_focused___button(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_focused___button_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, get_style_focused___button_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
};

//
// Style: Device Card
//

void init_style_device_card_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_pad_left(style, 5);
    lv_style_set_pad_right(style, 5);
    lv_style_set_pad_bottom(style, 2);
    lv_style_set_pad_top(style, 2);
};

lv_style_t *get_style_device_card_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_device_card_MAIN_DEFAULT(style);
    }
    return style;
};

void init_style_device_card_MAIN_FOCUS_KEY(lv_style_t *style) {
    lv_style_set_border_color(style, lv_color_hex(theme_colors[eez_flow_get_selected_theme_index()][0]));
};

lv_style_t *get_style_device_card_MAIN_FOCUS_KEY() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_device_card_MAIN_FOCUS_KEY(style);
    }
    return style;
};

void add_style_device_card(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_device_card_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, get_style_device_card_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
};

void remove_style_device_card(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_device_card_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, get_style_device_card_MAIN_FOCUS_KEY(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
};

//
//
//

void add_style(lv_obj_t *obj, int32_t styleIndex) {
    typedef void (*AddStyleFunc)(lv_obj_t *obj);
    static const AddStyleFunc add_style_funcs[] = {
        add_style_focused___dropdown,
        add_style_focused___switch,
        add_style_default_main_content,
        add_style_default_main_menu_panel,
        add_style_default_main_content__panel_,
        add_style_alert_card,
        add_style_focused___button,
        add_style_device_card,
    };
    add_style_funcs[styleIndex](obj);
}

void remove_style(lv_obj_t *obj, int32_t styleIndex) {
    typedef void (*RemoveStyleFunc)(lv_obj_t *obj);
    static const RemoveStyleFunc remove_style_funcs[] = {
        remove_style_focused___dropdown,
        remove_style_focused___switch,
        remove_style_default_main_content,
        remove_style_default_main_menu_panel,
        remove_style_default_main_content__panel_,
        remove_style_alert_card,
        remove_style_focused___button,
        remove_style_device_card,
    };
    remove_style_funcs[styleIndex](obj);
}