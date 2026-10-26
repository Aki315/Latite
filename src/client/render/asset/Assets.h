#pragma once
#include "Asset.h"
#include <vector>

class Assets final {
public:
    Assets();
    ~Assets();
    Assets(Assets&) = delete;
    Assets(Assets&&) = delete;

    Asset latiteLogo;
    Asset searchIcon;
    Asset arrowIcon;
    Asset xIcon;
    Asset hudEditIcon;
    Asset arrowBackIcon;
    Asset cogIcon;
    Asset checkmarkIcon;
    Asset logoWhite;
    Asset document;
    Asset shieldOn;  // Shield Indicator: a working shield.
    Asset shieldOff; // Shield Indicator: a disabled shield.

    void loadAll();
    void unloadAll();

private:
    std::vector<Asset*> allAssets {};
};
