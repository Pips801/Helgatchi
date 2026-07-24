#pragma once
#include "event_bus.h"

class SerialConsole {
public:
    void begin(EventBus& bus);
    void tick();   // call every loop()

private:
    // Large enough to hold a full `rule save <json>` line on one input line.
    static constexpr uint16_t BUF_LEN = 2048;

    void _dispatch(char* line);
    void _cmdHelp();

    // Multi-subcommand verbs. Each prints its own usage when called with no
    // args; routes to subcommands otherwise.
    void _cmdSetting(char* args);   // list / set / save / reset
    void _cmdAlert(char* args);     // list / raise / ack / clear
    void _cmdLed(char* args);       // list / play / off / bright
    void _cmdVibe(char* args);      // list / play / off
    void _cmdRule(char* args);      // list / show / create / add / rm / delete / enable / disable / reload / stats
    void _cmdParty(char* args);     // on [secs] / off
    void _cmdAdmin(char* args);     // unlock / lock / party / msg / led / beacon / stopall / menu
    void _cmdScan(char* args);      // list / inject / clear
    void _cmdVendor(char* args);    // stats / oui / mfg / search
    void _cmdPower(char* args);     // sleep / sleepscreen / reboot / shipping

    // Singletons (no subcommands).
    void _cmdBus(char* args);
    void _cmdStats();
    void _cmdBattery();
    void _cmdSelftest();
    void _cmdVer();      // machine-readable version line (for the web companion)
    void _cmdUpdate();   // paint the "updating" screen + ack, before a web flash
    void _cmdWebinfo();  // one-shot JSON bootstrap (ver+led+vibe+rules) for the web app

    EventBus* _bus                       = nullptr;
    char      _buf[BUF_LEN];
    uint16_t  _pos                       = 0;   // BUF_LEN > 255, so not uint8_t
    bool      _was_connected             = false;
    uint32_t  _last_seen_connected_ms    = 0;   // hysteresis against CDC `bool Serial` blips
};

extern SerialConsole g_console;
