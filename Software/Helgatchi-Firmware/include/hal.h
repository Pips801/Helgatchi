#pragma once
#include "lgfx_config.h"
#include <FastLED.h>
#include "event_bus.h"
#include <esp_timer.h>

// ---------------------------------------------------------------------------
// GPIO pin assignments (from GPIO.md)
// NOTE: GPIO43 is ESP32-S3 UART0 TX. On XIAO ESP32-S3, Serial = USB CDC,
//       so UART0 is unused and GPIO43 is free for button input.
// ---------------------------------------------------------------------------
static constexpr uint8_t PIN_SPI_DC   =  1;
static constexpr uint8_t PIN_LED_DATA =  2;
static constexpr uint8_t PIN_SPI_BL   =  3;  // backlight — PWM controlled by HAL
static constexpr uint8_t PIN_VIBRATE  =  4;
static constexpr uint8_t PIN_VSENSE   =  5;
static constexpr uint8_t PIN_BTN_1    =  6;  // button matrix line A
static constexpr uint8_t PIN_BTN_2    = 43;  // button matrix line B
static constexpr uint8_t PIN_SPI_CS   = 44;
static constexpr uint8_t PIN_SPI_SCK  =  7;
static constexpr uint8_t PIN_SPI_RST  =  8;
static constexpr uint8_t PIN_SPI_MOSI =  9;

// ---------------------------------------------------------------------------
// HAL tuning constants
// ---------------------------------------------------------------------------
static constexpr uint8_t  HAL_NUM_LEDS       =  6;
static constexpr uint16_t HAL_LONG_PRESS_MS  =  600;
static constexpr uint16_t HAL_HOLD_MS        = 2500;   // center-hold-to-sleep (wake holds live in power_manager)
static constexpr uint8_t  HAL_DEBOUNCE_MS    =   20;

// Left/Right hold-to-repeat (key auto-repeat). Holding a nav button past
// HAL_REPEAT_DELAY_MS re-emits its EV_BTN_* at a steady HAL_REPEAT_INTERVAL_MS,
// so long lists — the device list — scroll without 100+ discrete presses. Each
// repeat is a verbatim press, so it navigates and ticks identically; at ~6/s the
// tick is a discrete click, not a buzz. Slow it (raise the interval) or drop the
// per-step tick in the consumers if it turns out to be too much.
static constexpr uint16_t HAL_REPEAT_DELAY_MS    = 400;   // hold this long before auto-repeat starts
static constexpr uint16_t HAL_REPEAT_INTERVAL_MS = 167;   // steady repeat interval (~6 steps/sec)
static constexpr uint8_t  HAL_BTN_POLL_MS    =    5;   // fixed button sample rate (esp_timer), decoupled from loop cadence
static constexpr uint8_t  HAL_BL_LEDC_CH     =  0;   // LEDC channel for backlight PWM
static constexpr uint8_t  HAL_VIBE_LEDC_CH   =  1;   // LEDC channel for vibration motor PWM

// Backlight PWM levels mapped to ScreenBrightness enum indices
//                                            MIN  LOW  MED  HIGH  MAX
static constexpr uint8_t HAL_BL_LEVELS[]  = {  15,  50, 110,  180, 255 };
// LED global brightness levels mapped to LEDBrightness enum indices
static constexpr uint8_t HAL_LED_LEVELS[] = {  20,  60, 130,  255 };

// ---------------------------------------------------------------------------

class HAL : public IEventHandler {
public:
    // Initialise all peripherals and subscribe to relevant bus events.
    void begin(EventBus& bus);

    // Service the USB-attach (SOF) detector. Call every loop(). Button polling
    // runs on its own esp_timer (see _btnTimerCb) so input sampling isn't
    // coupled to loop/render cadence; tick() only falls back to polling if that
    // timer failed to create.
    void tick();

    // --- LEDs (call showLEDs() is implicit on each set call) ---
    void setLED(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
    void setAllLEDs(uint8_t r, uint8_t g, uint8_t b);
    void clearLEDs();

    // Push a full N-LED frame in one FastLED.show() call. Used by LedService
    // for animated patterns where we'd otherwise pay N show()s per frame.
    void writeLEDFrame(const CRGB frame[HAL_NUM_LEDS]);

    // --- Vibration (PWM-driven, 0–255 duty) ---
    // Raw level control. VibeService owns timing + patterns; callers that want
    // simple feedback should go through it instead of poking these directly.
    void setVibrate(uint8_t intensity);
    void stopVibrate();

    // --- Display ---
    LGFX& tft()                          { return _tft; }
    void  setBacklight(uint8_t val);  // 0–255

    // Display state transitions, all driven by PowerManager. begin() leaves the
    // screen OFF — wake/dim/sleepDisplay actually drive the backlight. The
    // dim/sleep variants are transient (still in interactive use): for deep
    // sleep entry call prepareForSleep() instead.
    void  wakeDisplay();    // BL → user's stored brightness
    void  dimDisplay();     // BL → MIN level (last 5 s before sleep)
    void  sleepDisplay();   // BL → 0 (silent scan window, no user-visible UI)

    // Call right before esp_deep_sleep_start(). Detaches LEDC from the
    // backlight pin, drives it LOW, and locks the pad state through deep
    // sleep — without this, the LEDC peripheral powers down during sleep
    // and the backlight reverts on, leaving the last frame visibly burnt-in.
    void  prepareForSleep();

    // Turn peripherals (motor / LEDs / backlight) off ahead of a software
    // reset (ESP.restart). Unlike prepareForSleep it doesn't hold pads or
    // sleep the panel — the reboot re-inits them. Prevents LEDC from holding
    // its last PWM duty (motor / backlight) across the reset until begin()
    // re-inits it, which is what left the motor buzzing through a reboot.
    void  prepareForReboot();

    // --- Power sensing ---
    // Returns raw voltage at the VSENSE ADC pin in millivolts (VBATT/2 divider).
    uint16_t readVsenseMv();

    // Returns true if USB is physically attached.
    // Updated non-blocking every 2 ms in tick() via SOF frame detection.
    // Does NOT detect wall chargers (no SOF packets from dumb chargers).
    bool usbAttached() const { return _usb_attached; }

    // IEventHandler — reacts to EV_SETTINGS_CHANGED (brightness) and
    // EV_ALERT_RAISED (buzz if vibration enabled).
    void onEvent(const Event& e) override;

private:
    void _initDisplay();
    void _initLEDs();
    void _pollButtons();
    static void _btnTimerCb(void* arg);   // esp_timer trampoline → _pollButtons()
    void _applyBrightnessSettings();

    struct BtnDebounce {
        bool     state        = false;
        bool     raw_prev     = false;
        bool     wake_swallow = false;   // press began on a dark screen — cycle emits no EV_BTN_*
        uint32_t stable_since = 0;
    };

    BtnDebounce _btn[3];          // [0]=Left  [1]=Right  [2]=Center
    uint32_t    _center_down_at    = 0;
    bool        _center_long_fired = false;
    bool        _center_hold_fired = false;

    // Left/Right hold-to-repeat: next-due timestamp per button ([0]=Left, [1]=Right).
    uint32_t    _lr_repeat_at[2]   = {0, 0};   // millis() the next auto-repeat is due; 0 = disarmed
    esp_timer_handle_t _btn_timer  = nullptr;   // periodic button-poll timer (HAL_BTN_POLL_MS)

    // True while the backlight is fully off (sleepDisplay). Sampled at each
    // press's falling edge to decide whether that press means "wake" (#53).
    // begin() leaves the screen off, so start asleep.
    bool     _display_asleep    = true;

    bool     _usb_attached      = false;
    bool     _serial_open       = false;   // (bool)Serial edge tracking for EV_SERIAL_*
    uint32_t _last_sof          = 0;
    uint32_t _last_sof_check_ms = 0;

    LGFX      _tft;
    CRGB      _leds[HAL_NUM_LEDS];
    EventBus* _bus = nullptr;
};

extern HAL g_hal;
