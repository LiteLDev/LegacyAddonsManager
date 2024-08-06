#pragma once

#include <ll/api/mod/NativeMod.h>

namespace legacy_addons_manager {

struct Addon {
    enum class Type { ResourcePack, BehaviorPack };
    std::string       name;
    std::string       description;
    Type              type;
    ll::data::Version version;
    std::string       uuid;
    std::string       directory;
    bool              enable = false;
};

class AddonsManager {
public:
    static bool install(std::string path);
    static bool uninstall(std::string nameOrUuid);

    static bool enable(std::string nameOrUuid);
    static bool disable(std::string nameOrUuid);

    static std::vector<Addon*> getAllAddons();
    static Addon*              findAddon(const std::string& nameOrUuid, bool fuzzy = false);
};

class LegacyAddonsManager {

public:
    static LegacyAddonsManager& getInstance();

    LegacyAddonsManager(ll::mod::NativeMod& self) : mSelf(self) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();

    bool enable();

    bool disable();

    // bool unload();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace legacy_addons_manager
