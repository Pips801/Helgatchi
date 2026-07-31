#pragma once

enum class PartyScreen {
    OVERVIEW,
    HELGA_MENU,
    OTHER,
};

// Tracks whether an active party has reached Overview and decides whether a
// subsequent screen change is a dismissal. Helga Menu is the one intentional
// off-Overview state: manual Helga playback returns there while automatic
// party state continues in the background. Every other settled departure keeps
// the existing manual-dismiss behavior.
class PartyNavigationState {
public:
    void reset() { _overview_settled = false; }

    bool shouldDismiss(PartyScreen screen) {
        if (screen == PartyScreen::OVERVIEW) {
            _overview_settled = true;
            return false;
        }
        if (screen == PartyScreen::HELGA_MENU) return false;
        return _overview_settled;
    }

    bool overviewSettled() const { return _overview_settled; }

private:
    bool _overview_settled = false;
};
