#pragma once

#include "event_ids.h"
#include "helga_animation.h"

class HelgaPlaybackState {
public:
    explicit HelgaPlaybackState(HelgaAnim initial = HELGA_IDLE);

    bool setAutomatic(HelgaAnim animation);
    bool startManual(HelgaAnim animation);
    bool stopManual();
    bool consumeExitButton(EventId event_id);

    bool manualActive() const { return _manual_active; }
    HelgaAnim automaticAnimation() const { return _automatic; }
    HelgaAnim visibleAnimation() const {
        return _manual_active ? _manual : _automatic;
    }

private:
    HelgaAnim _automatic;
    HelgaAnim _manual = HELGA_IDLE;
    bool _manual_active = false;
};
