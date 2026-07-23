#include "hal.h"
#include "settings_service.h"
#include <Arduino.h>
#include "soc/usb_serial_jtag_struct.h"
#include <driver/gpio.h>

HAL g_hal;


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void HAL::begin(EventBus& bus) {
    _bus = &bus;

    // Seed the SOF baseline from the live register. It survives resets holding
    // the pre-reset frame count (nonzero after flashing or any prior USB
    // session), so comparing against an initial 0 made the FIRST 100 ms check
    // in tick() always read "attached" — a phantom USB icon on every boot,
    // host or not. Baselined here, the first check only fires if SOF frames
    // actually advanced since boot, which requires a live host.
    _last_sof = USB_SERIAL_JTAG.fram_num.sof_frame_index;

    // Release any GPIO pad holds left over from a prior deep-sleep cycle (see
    // prepareForSleep). Without this, the held pins would stay locked LOW —
    // LEDC PWM would silently fail to drive the backlight, and FastLED would
    // be unable to push data to the LED chain.
    gpio_hold_dis((gpio_num_t)PIN_SPI_BL);
    gpio_hold_dis((gpio_num_t)PIN_LED_DATA);
    gpio_deep_sleep_hold_dis();

    // Drive the backlight + LED-data pins to a known-LOW state IMMEDIATELY,
    // before _initDisplay / _initLEDs run. Reasons:
    //   • PIN_SPI_BL (GPIO3) is a strapping pin and has an internal pull-up
    //     enabled by ROM. After gpio_hold_dis releases the latch, the pad
    //     enters HI-Z and the pull-up drives it HIGH — the backlight visibly
    //     flashes on for the duration of _tft.init() (which has its own
    //     ~150ms delays) until ledcAttachPin re-grabs the pin with duty 0.
    //   • PIN_LED_DATA (GPIO2) has no peripheral driver attached on wake.
    //     A floating SK6805 data line + radio noise during boot can be
    //     interpreted as data — typically as 0xFFFFFF (white flash).
    pinMode(PIN_SPI_BL, OUTPUT);
    digitalWrite(PIN_SPI_BL, LOW);
    pinMode(PIN_LED_DATA, OUTPUT);
    digitalWrite(PIN_LED_DATA, LOW);

    pinMode(PIN_BTN_1,   INPUT_PULLUP);
    pinMode(PIN_BTN_2,   INPUT_PULLUP);
    pinMode(PIN_VSENSE,  INPUT);
    analogSetAttenuation(ADC_11db);   // set once — VSENSE is the only ADC pin, no need to reconfigure per sample

    // Seed button debounce state to the current physical state. On a wake from
    // deep sleep the user is still holding CENTER (the wake handshake required
    // it), so without seeding, the first poll sees a fresh press → fires
    // EV_BTN_CENTER_SHORT the instant they release (entering the first menu
    // item) and could even fire EV_BTN_CENTER_HOLD (sleeping the device right
    // back). Seeding the held state means no falling edge fires; events resume
    // only after a real release + re-press. Harmless on cold/timer boots where
    // nothing is held (seeds all-false, same as the struct defaults).
    delayMicroseconds(200);  // let pullups settle before the seed read
    {
        const uint32_t now   = millis();
        const bool     raw_a = !digitalRead(PIN_BTN_1);
        const bool     raw_b = !digitalRead(PIN_BTN_2);
        const bool     seed[3] = { raw_a && !raw_b, !raw_a && raw_b, raw_a && raw_b };
        for (int i = 0; i < 3; i++) {
            _btn[i].state        = seed[i];
            _btn[i].raw_prev     = seed[i];
            _btn[i].stable_since = now;
        }
        if (seed[2]) {
            // Center held at boot — suppress the release-SHORT and the
            // long/hold fires for this initial hold.
            _center_down_at    = now;
            _center_long_fired = true;
            _center_hold_fired = true;
        }
    }

    // Button sampling runs on a fixed HAL_BTN_POLL_MS esp_timer, not the main
    // loop, so a slow render frame can't widen the poll gap and alias out fast
    // taps. The callback posts EV_BTN_* / EV_UI_ACTIVITY (bus.post is thread-
    // safe); the UI still consumes them on dispatch(). Created AFTER the
    // debounce seed above so no poll fires before the initial state is set.
    const esp_timer_create_args_t btn_args = {
        .callback        = &HAL::_btnTimerCb,
        .arg             = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "btnpoll",
    };
    if (esp_timer_create(&btn_args, &_btn_timer) == ESP_OK) {
        esp_timer_start_periodic(_btn_timer, (uint64_t)HAL_BTN_POLL_MS * 1000);
    } else {
        Serial.println("[hal] btn-poll esp_timer create failed — falling back to loop polling");
        _btn_timer = nullptr;
    }

    // Vibration motor on its own LEDC channel. 20 kHz keeps PWM out of the
    // audible range so the motor doesn't whine; 8-bit gives 0–255 intensity.
    ledcSetup(HAL_VIBE_LEDC_CH, 20000, 8);
    ledcAttachPin(PIN_VIBRATE, HAL_VIBE_LEDC_CH);
    ledcWrite(HAL_VIBE_LEDC_CH, 0);

    _initDisplay();
    _initLEDs();
    // NOTE: brightness intentionally NOT applied here. PowerManager decides
    // whether to call wakeDisplay() based on the wake cause — TIMER (silent
    // scan) wakes leave the screen off entirely. _applyBrightnessSettings()
    // still runs on EV_SETTINGS_CHANGED so live adjustments work.

    bus.subscribe(EV_SETTINGS_CHANGED, this);
    // EV_ALERT_RAISED is handled by VibeService now — it owns the haptic
    // pattern catalog and decides what to play.
}

void HAL::tick() {
    // Buttons normally sample on _btn_timer; poll here only if that timer
    // failed to create (see begin()), so input never dies entirely.
    if (!_btn_timer) _pollButtons();

    // SOF frames arrive every 1 ms while connected to a USB host.
    // Check every 100 ms — if the counter moved at all in the window we're
    // attached. The previous 2 ms window was too tight: hosts occasionally
    // pause SOFs (heavy traffic, brief suspend) for more than 2 ms, which
    // would falsely flip _usb_attached to false and cascade through to
    // _is_charging, breaking sleep inhibition.
    uint32_t now = millis();
    if (now - _last_sof_check_ms >= 100) {
        uint32_t sof     = USB_SERIAL_JTAG.fram_num.sof_frame_index;
        bool attached    = (sof != _last_sof);
        if (attached != _usb_attached && _bus) {
            _bus->post(attached ? EV_USB_CONNECTED : EV_USB_DISCONNECTED);
        }
        _usb_attached      = attached;
        _last_sof          = sof;
        _last_sof_check_ms = now;

        // USB-CDC port open/close (host attaches/detaches a serial monitor).
        // Sampled on the same 100 ms cadence as SOF; (bool)Serial is a cheap
        // DTR read. Edge-emitted so services can react without polling.
        bool serial_open = (bool)Serial;
        if (serial_open != _serial_open && _bus) {
            _bus->post(serial_open ? EV_SERIAL_CONNECTED : EV_SERIAL_DISCONNECTED);
        }
        _serial_open = serial_open;
    }

}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void HAL::_initDisplay() {
    _tft.init();
    _tft.setRotation(1);  // landscape: logical 280 x 240
    _tft.fillScreen(TFT_BLACK);

    // Backlight via LEDC PWM (arduino-esp32 v2 channel-based API).
    //
    // Leave duty at 0 here — PowerManager decides whether to wake the screen
    // based on the wake cause (TIMER = silent scan, leave off; everything
    // else = wakeDisplay()). Explicit ledcWrite(0) so we don't depend on
    // arduino-esp32's LEDC default behaviour staying 0 across releases.
    ledcSetup(HAL_BL_LEDC_CH, 5000, 8);
    ledcAttachPin(PIN_SPI_BL, HAL_BL_LEDC_CH);
    ledcWrite(HAL_BL_LEDC_CH, 0);
}

void HAL::setBacklight(uint8_t val) {
    ledcWrite(HAL_BL_LEDC_CH, val);
}

void HAL::wakeDisplay() {
    uint8_t bl = g_settings.get(SKEY_SCREEN_BRIGHTNESS);
    if (bl >= SCREEN_BRIGHTNESS_COUNT) bl = SCREEN_BRIGHTNESS_HIGH;
    ledcWrite(HAL_BL_LEDC_CH, HAL_BL_LEVELS[bl]);
}

void HAL::dimDisplay() {
    ledcWrite(HAL_BL_LEDC_CH, HAL_BL_LEVELS[SCREEN_BRIGHTNESS_MIN]);
}

void HAL::sleepDisplay() {
    ledcWrite(HAL_BL_LEDC_CH, 0);
}

void HAL::prepareForSleep() {
    // Stop the vibration motor unconditionally — if a haptic pattern was
    // mid-play when sleep was triggered, we don't want to wake on a buzz.
    stopVibrate();

    // Put the LCD controller into its own sleep mode. The panel stops driving
    // pixels and drops controller current to ~50 µA. Cheap and reduces the
    // chance of a faint visible image from any residual VDD on the panel.
    _tft.sleep();

    // Backlight: detach LEDC, drive LOW, lock through sleep. ledcWrite(0)
    // alone isn't enough — when esp_deep_sleep_start() powers LEDC down the
    // pin loses its driver, drifts HIGH, and the backlight reverts on.
    ledcDetachPin(PIN_SPI_BL);
    pinMode(PIN_SPI_BL, OUTPUT);
    digitalWrite(PIN_SPI_BL, LOW);

    // LED data: take the pin away from FastLED's RMT routing, drive LOW, and
    // lock through sleep. Without this lock, the SK6805 data line floats for
    // the entire sleep window (tens of seconds) and EMI/noise drifts the
    // controllers' shift register into a bad state — visible as a single
    // white flash on the next silent (TIMER) wake when FastLED takes over.
    pinMode(PIN_LED_DATA, OUTPUT);
    digitalWrite(PIN_LED_DATA, LOW);

    // Lock both pads LOW for the duration of deep sleep. Released in begin().
    gpio_hold_en((gpio_num_t)PIN_SPI_BL);
    gpio_hold_en((gpio_num_t)PIN_LED_DATA);
    gpio_deep_sleep_hold_en();
}

void HAL::prepareForReboot() {
    // Peripheral shutdown ahead of a software reset (ESP.restart). Unlike
    // prepareForSleep this does NOT hold pads or sleep the panel — the reboot
    // re-inits everything a few hundred ms later. Its job is to clear the state
    // that would otherwise persist across the reset: LEDC keeps driving its
    // last PWM duty through a software reset until HAL re-inits it at boot, so
    // a motor or backlight left ON stays ON for the whole boot window. That's
    // what left the vibration motor buzzing through a reboot.
    stopVibrate();                   // motor duty -> 0
    clearLEDs();                     // all LEDs off (RMT still owns the pin here)
    ledcWrite(HAL_BL_LEDC_CH, 0);    // backlight off — hide the frozen frame during boot
}

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------

void HAL::_initLEDs() {
    // WS2813 chipset for the Würth WL-ICLED (1313210530000) side-view RGB
    // IC-LEDs. FastLED's WS2813 timing is <T1,T2,T3> = 375/500/375 ns, which
    // lands inside every window of the WL-ICLED datasheet (T0H 150/300/450,
    // T1H 750/900/1050, T0L 750/900/1050, T1L 150/300/450, period 900/1200/1500):
    //   T0H = T1        = 375 ns   T1H = T1+T2 = 875 ns
    //   T0L = T2+T3     = 875 ns   T1L = T3    = 375 ns   period = 1250 ns
    //
    // Do NOT use SK6812 here: its T1L (T3 = 500 ns) exceeds the WL-ICLED T1L
    // max of 450 ns and its T1H sits on the 750 ns minimum — out of spec, which
    // is why the bits looked off. WS2812 also fits the Würth windows, but its
    // 250 ns T0H is below the 300 ns minimum of the legacy SK6805 parts on
    // R2.8+ boards; WS2813's 375 ns T0H keeps those working too.
    FastLED.addLeds<WS2813, PIN_LED_DATA, GRB>(_leds, HAL_NUM_LEDS);
    FastLED.setBrightness(HAL_LED_LEVELS[1]);  // default: MEDIUM
    FastLED.clear(true);
}

void HAL::setLED(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx >= HAL_NUM_LEDS) return;
    _leds[idx] = CRGB(r, g, b);
    FastLED.show();
}

void HAL::setAllLEDs(uint8_t r, uint8_t g, uint8_t b) {
    fill_solid(_leds, HAL_NUM_LEDS, CRGB(r, g, b));
    FastLED.show();
}

void HAL::clearLEDs() {
    FastLED.clear(true);
}

void HAL::writeLEDFrame(const CRGB frame[HAL_NUM_LEDS]) {
    for (uint8_t i = 0; i < HAL_NUM_LEDS; i++) _leds[i] = frame[i];
    FastLED.show();
}

// ---------------------------------------------------------------------------
// Vibration
// ---------------------------------------------------------------------------

void HAL::setVibrate(uint8_t intensity) {
    ledcWrite(HAL_VIBE_LEDC_CH, intensity);
}

void HAL::stopVibrate() {
    ledcWrite(HAL_VIBE_LEDC_CH, 0);
}

// ---------------------------------------------------------------------------
// Power sensing
// ---------------------------------------------------------------------------


uint16_t HAL::readVsenseMv() {
    // Attenuation is set once in begin(). 4 calibrated samples averaged — enough
    // given the downstream EMA in PowerManager::_sampleBattery, and keeps the
    // ~30 s battery read well under a frame (see phase_power teleplot).
    int32_t sum = 0;
    for (int i = 0; i < 4; i++) sum += analogReadMilliVolts(PIN_VSENSE);
    return (uint16_t)(sum / 4);
}

// ---------------------------------------------------------------------------
// Button polling (2×2 diode matrix: GPIO6 + GPIO43 → Left / Right / Center)
//
// Matrix decode:
//   GPIO6 LOW  only  → Left
//   GPIO43 LOW only  → Right
//   Both LOW         → Center  (diodes prevent cross-drive)
// ---------------------------------------------------------------------------

void HAL::_btnTimerCb(void* arg) {
    static_cast<HAL*>(arg)->_pollButtons();
}

void HAL::_pollButtons() {
    uint32_t now = millis();

    bool raw_a = !digitalRead(PIN_BTN_1);
    bool raw_b = !digitalRead(PIN_BTN_2);

    bool raw[3] = {
        raw_a && !raw_b,   // Left
        !raw_a && raw_b,   // Right
        raw_a &&  raw_b,   // Center
    };

    for (int i = 0; i < 3; i++) {
        if (raw[i] != _btn[i].raw_prev) {
            _btn[i].stable_since = now;
            _btn[i].raw_prev     = raw[i];
        }

        if ((now - _btn[i].stable_since) < HAL_DEBOUNCE_MS) continue;

        bool was       = _btn[i].state;
        _btn[i].state  = raw[i];

        if (!was && _btn[i].state) {
            // Falling edge (press)
            _bus->post(EV_UI_ACTIVITY);
            if      (i == 0) _bus->post(EV_BTN_LEFT);
            else if (i == 1) _bus->post(EV_BTN_RIGHT);
            else {
                _center_down_at    = now;
                _center_long_fired = false;
                _center_hold_fired = false;
            }
        } else if (was && !_btn[i].state) {
            // Rising edge (release)
            if (i == 2 && !_center_long_fired) {
                _bus->post(EV_BTN_CENTER_SHORT);
            }
        }
    }

    // Long-press detection for center button. Two thresholds:
    //   HAL_LONG_PRESS_MS (~600 ms)  → EV_BTN_CENTER_LONG  (back / pop screen)
    //   HAL_HOLD_MS       (~2000 ms) → EV_BTN_CENTER_HOLD  (sleep on main menu)
    if (_btn[2].state) {
        const uint32_t held = now - _center_down_at;
        if (!_center_long_fired && held >= HAL_LONG_PRESS_MS) {
            _center_long_fired = true;
            _bus->post(EV_BTN_CENTER_LONG);
        }
        if (!_center_hold_fired && held >= HAL_HOLD_MS) {
            _center_hold_fired = true;
            _bus->post(EV_BTN_CENTER_HOLD);
        }
    }
}

// ---------------------------------------------------------------------------
// Settings-driven brightness
// ---------------------------------------------------------------------------

void HAL::_applyBrightnessSettings() {
    uint8_t bl  = g_settings.get(SKEY_SCREEN_BRIGHTNESS);
    uint8_t led = g_settings.get(SKEY_LED_BRIGHTNESS);

    if (bl  >= SCREEN_BRIGHTNESS_COUNT) bl  = SCREEN_BRIGHTNESS_HIGH;
    if (led >= LED_BRIGHTNESS_COUNT)    led = LED_BRIGHTNESS_MEDIUM;

    ledcWrite(HAL_BL_LEDC_CH, HAL_BL_LEVELS[bl]);
    FastLED.setBrightness(HAL_LED_LEVELS[led]);
    FastLED.show();
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void HAL::onEvent(const Event& e) {
    switch (e.id) {
        case EV_SETTINGS_CHANGED:
            if (e.data.settings.mask & SMASK_UI) {
                _applyBrightnessSettings();
            }
            break;

        default:
            break;
    }
}
