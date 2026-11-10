// The SD card, open only while it's used (as in Melody: an idle sysmodule
// holding a file system connection kept other apps from starting). While a
// Card is around, sdmc:/ works with stdio.
#pragma once
#include <switch.h>

namespace card {
bool acquire();  // open the card (counted); false: it can't be used now
void release();
}  // namespace card

struct Card {
    bool ok;
    Card() : ok(card::acquire()) {}
    ~Card() {
        if (ok) card::release();
    }
    Card(const Card&) = delete;
    Card& operator=(const Card&) = delete;
};
