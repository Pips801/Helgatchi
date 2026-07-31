# Screens

This document describes the current EEZ Studio pages, generated LVGL objects,
and firmware code that supplies runtime behavior.

## Source of truth and generation workflow

- **EEZ Studio project**: `Software/UI/Helgatchi UI.eez-project`.
- **Generated destination**: EEZ Studio Build writes
  `Software/Helgatchi-Firmware/src/UI/`.
- **Runtime wiring**: screen-specific firmware modules under
  `Software/Helgatchi-Firmware/src/`, with input and display setup in
  `Software/Helgatchi-Firmware/src/ui_controller.cpp`.

Files under `Software/Helgatchi-Firmware/src/UI/` are generated. Do not edit
them directly. Make page, user-widget, group, style, and flow changes in the
EEZ Studio project, invoke Build, then review the generated diff and compile
the firmware.

Generated object names are exposed through `objects` in `src/UI/screens.h`.
Runtime code attaches callbacks to those objects after `g_ui.begin()` has
called `ui_init()` and created the generated screens.

## Current generated screen inventory

`src/UI/screens.h` currently defines these 18 EEZ pages and screen IDs:

| # | EEZ page | Generated screen ID |
|---:|---|---|
| 1 | Main Menu | `SCREEN_ID_MAIN_MENU` |
| 2 | Tutorial Splash Screen | `SCREEN_ID_TUTORIAL_SPLASH_SCREEN` |
| 3 | Tutorial | `SCREEN_ID_TUTORIAL` |
| 4 | Settings | `SCREEN_ID_SETTINGS` |
| 5 | Info | `SCREEN_ID_INFO` |
| 6 | Screen Template | `SCREEN_ID_SCREEN_TEMPLATE` |
| 7 | Alerts | `SCREEN_ID_ALERTS` |
| 8 | Devices | `SCREEN_ID_DEVICES` |
| 9 | Device Updating | `SCREEN_ID_DEVICE_UPDATING` |
| 10 | Debug Info | `SCREEN_ID_DEBUG_INFO` |
| 11 | Overview | `SCREEN_ID_OVERVIEW` |
| 12 | Power Menu | `SCREEN_ID_POWER_MENU` |
| 13 | Power Action Screen | `SCREEN_ID_POWER_ACTION_SCREEN` |
| 14 | Admin Menu | `SCREEN_ID_ADMIN_MENU` |
| 15 | Foxhunting Menu | `SCREEN_ID_FOXHUNTING_MENU` |
| 16 | Rules | `SCREEN_ID_RULES` |
| 17 | Helga Menu | `SCREEN_ID_HELGA_MENU` |
| 18 | LED Modes Menu | `SCREEN_ID_LED_MODES_MENU` |

These are generated screens, not a list of future placeholders. Runtime
modules populate and control dynamic content on pages such as Alerts, Devices,
Overview, Rules, Settings, Debug Info, and the power screens.

## Reusable top bar

The EEZ project defines `Top Bar` as a reusable user widget and instances it
across the applicable pages. Its left and right expressions read the EEZ global
variables `status_icons` and `battery_status`; the center text is supplied by
each user-widget instance.

`DisplayService` updates those EEZ global variables from scan, alert, USB,
serial, charging, and battery state. EEZ Flow propagates the values to every
top-bar instance. To add or change a top bar, edit the page and `Top Bar` user
widget in EEZ Studio and run Build. Add runtime code only when the new page
needs behavior beyond the existing global expressions.

## Startup and navigation ownership

`UIController::begin()` initializes LVGL and the display, then calls
`ui_init()`. EEZ creates all pages and loads Main Menu as the default. Firmware
then creates the keypad input and attaches it to `groups.UINavigation`. On first
boot or after the tutorial flag is reset, firmware loads
`objects.tutorial_splash_screen` instead.

Page-card and button navigation authored in the EEZ flow graph uses Change
Screen actions and the EEZ page stack. Firmware uses
`eez_flow_set_screen()`, `eez_flow_push_screen()`, and
`eez_flow_pop_screen()` when navigation depends on runtime state.

Each generated page's `LV_EVENT_SCREEN_LOAD_START` handler clears and
repopulates the single `groups.UINavigation` group with the objects assigned to
that group in EEZ Studio. The keypad input remains attached to this group.
Navigation order therefore comes from the explicit EEZ group order, not from a
runtime traversal of the widget tree.

Main Menu is the one page with extra focus persistence in `ui_controller.cpp`:
firmware saves the focused card on unload and restores it after the generated
load handler rebuilds `groups.UINavigation`. Screen-specific modules register
their own load/unload callbacks as needed; there is no universal firmware
screen callback that owns all pages.

## Main Menu

The horizontal scroll-snap carousel is
`main_menu_scrolling_container`. Its generated navigation order is:

1. `overview_panel`
2. `helga_panel`
3. `led_modes_panel`
4. `devices_panel`
5. `alerts_panel`
6. `rules_panel`
7. `games_panel`
8. `settings_panel`
9. `info_panel`
10. `admin_panel`
11. `power_panel`

The `helga_panel` card is always present; admin lock state affects the admin
entry, not Helga.

To add a menu entry:

1. Edit the Main Menu page in `Software/UI/Helgatchi UI.eez-project`.
2. Add the panel to the EEZ `UINavigation` group in the intended focus order.
3. Connect its `CLICKED` output to a Change Screen action targeting an existing
   or new EEZ page, with the intended page-stack behavior.
4. Run EEZ Studio Build and review the project and generated output.
5. Add a runtime module or callback only if the destination needs dynamic
   behavior that the EEZ flow does not provide.

## Helga Menu

The always-visible `helga_panel` card opens the Helga screen from the Main Menu
carousel, whether admin mode is locked or unlocked. The `helga_menu` page
contains only `helga_animation_dropdown`; there is no preview widget.

The dropdown is populated at runtime with these display names, in this exact
order:

1. Idle
2. Idle Fidget
3. Idle Sneeze
4. Idle Wag
5. Idle Head Tilt
6. Sit
7. Walk
8. Party
9. Dance
10. Sniff
11. Alert
12. Brush
13. Sleep

Press center to open the dropdown, use left/right to choose an option, and press
center again to commit it. Committing the already-selected option also starts
playback; a changed selection is not required.

Manual playback runs the chosen animation full-screen on Overview. Automatic
status text is hidden during manual playback. The first subsequent physical
button action (left, right, center-short, center-long, or center-hold) is
consumed solely to stop playback and return to Helga Menu, so that action does
not also navigate or operate the dropdown. On return, the prior dropdown
selection is retained and the dropdown has focus.

Display dimming, display-off, and deep sleep are inhibited only while manual
playback is active. After manual playback exits, normal power behavior resumes;
ordinary Overview entry again shows the latest automatic animation and status.

## LED Modes menu

The always-visible `led_modes_panel` card opens `led_modes_menu` with stack
navigation. The screen contains only `led_mode_dropdown`. Firmware populates
Automatic followed by the 11 `LedPatternInfo::display_name` values.

Center opens/commits the dropdown and left/right change the highlighted option.
A commit applies immediately without screen navigation; recommitting a manual
option restarts its phase. Normal long-center back navigation preserves the
selection for the current boot.

Automatic clears the RAM-only manual layer. Off is a valid black manual layer.
Broadcast, foxhunt, and alert/party layers temporarily preempt manual output.
Manual mode inhibits automatic dim/sleep; explicit screen-off remains available.
Every reboot begins in Automatic.

## Settings

The Settings page is authored in EEZ Studio and exposes its widgets through
`objects` in the generated header. `SettingsScreen::begin()` registers
`LV_EVENT_VALUE_CHANGED` callbacks after UI creation. `_populate()` paints
current values from `SettingsService` on screen load and on
`EV_SETTINGS_CHANGED`, while `_inhibit` prevents that paint from feeding back
as user changes.

Current dropdown strings generated from the EEZ project are:

- Screen and LED brightness: `Low\nMedium\nHigh\nMax`.
- Performance mode: `Power Saver\nBalanced\nPerformance\nAlways-on`.
- Debug level: `Info\nHigh\nRender\nScan\nPerf\nTeleplot`.

The callbacks write screen-brightness, LED-brightness, and debug-level indexes
directly. As currently generated, screen-brightness indexes 0 through 3
therefore select `SCREEN_BRIGHTNESS_MIN` through `SCREEN_BRIGHTNESS_HIGH`;
`SCREEN_BRIGHTNESS_MAX` has no dropdown index. LED-brightness indexes cover its
four-value enum, and debug-level indexes cover its six-value enum. Performance
mode uses the explicit `kPerfToIdx` and `kIdxToPerf` translation in
`settings_screen.cpp`; it must not be treated as a direct enum-index mapping.

The BLE and Wi-Fi switches combine into the `SKEY_SCAN_MODE` bitmask. Other
switch callbacks write their corresponding alert, scan, debug, serial, USB, and
charging settings. `show_debug_options_switch` controls visibility of the debug
rows; hidden ancestors are skipped by LVGL group focus.

`reset_device_button` and `ship_device_button` are wired in
`settings_screen.cpp` to the Power Action flow. Debug Info and Restart Tutorial
navigation are authored in the EEZ flow graph.

## Dynamic runtime pages

Generated pages provide the static containers and screen objects; firmware
modules supply live content and actions:

- `alerts_screen.cpp` renders active, unacknowledged `AlertRecord` cards into
  `objects.alert_container`. Acknowledging an alert removes its record and
  card; there is no historical-alert store or view.
- `devices_screen.cpp` maintains the device-list recycler in
  `objects.devices_container` and owns device-detail modal navigation.
- `rules_screen.cpp` renders tag and individual-rule cards into
  `objects.rules_container`.
- `overview_screen.cpp` drives the Helga animation object and status label on
  `objects.overview`.
- `debug_screen.cpp`, `admin_service.cpp`, `foxhunting_screen.cpp`, and
  `power_menu_screen.cpp` attach behavior to their corresponding generated
  pages.
- `UIController::showUpdatingScreen()` loads `objects.device_updating` and
  forces a refresh before web flashing resets the device.

## Physical keypad contract

`UIController` queues LVGL keypad keys only for left/right and center-short
actions. `_kbd_read_cb()` emits each queued key as a pressed/released pair.
Center-long and center-hold actions are handled directly in
`UIController::onEvent()`. Current behavior is:

| Physical action | Runtime behavior |
|---|---|
| Left/right with dropdown open | `LV_KEY_UP` / `LV_KEY_DOWN` |
| Left/right while the group is editing | `LV_KEY_LEFT` / `LV_KEY_RIGHT` |
| Left/right in navigation mode | `LV_KEY_PREV` / `LV_KEY_NEXT` |
| Center short | `LV_KEY_ENTER` |
| Center long during party mode | Stop party mode and remain on Overview |
| Center long with a device-detail message box | Close the modal |
| Center long on Tutorial | Return to Tutorial Splash Screen |
| Center long on Overview | Go to Main Menu |
| Center long on other stacked pages | Pop the EEZ page stack |
| Center hold on Main Menu | Request sleep or screen-off |

Main Menu and Tutorial Splash Screen ignore ordinary center-long back
navigation. Before the table above is applied, an active manual Helga playback
gets first refusal: any physical button action is consumed by the manual
playback exit path described in the Helga section.

The open state of an LVGL dropdown is widget-local, so input routing checks
`lv_dropdown_is_open()` separately from `lv_group_get_editing()`.

## Cross-cutting display behavior

Every physical button event posts `EV_UI_ACTIVITY`; serial input also posts
activity. `PowerManager` uses it to update the last-activity time and wake the
display.

`UIController::tick()` skips EEZ Flow and LVGL timer work when rendering is
disabled. `PowerManager` enables rendering for ON/DIM display states and
disables it for OFF.

The LVGL performance monitor is compiled in, while
`LogService::applyPerfMonitor()` controls its runtime visibility from the debug
level. The setting is reapplied after UI initialization, on relevant settings
changes, and when the display returns to an enabled state.
