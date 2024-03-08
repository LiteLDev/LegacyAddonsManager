#pragma once

#include <ll/api/plugin/NativePlugin.h>

namespace LegacyAddonsManager {

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
    LegacyAddonsManager();

public:
    LegacyAddonsManager(LegacyAddonsManager&&)                 = delete;
    LegacyAddonsManager(const LegacyAddonsManager&)            = delete;
    LegacyAddonsManager& operator=(LegacyAddonsManager&&)      = delete;
    LegacyAddonsManager& operator=(const LegacyAddonsManager&) = delete;

    static LegacyAddonsManager& getInstance();

    [[nodiscard]] ll::plugin::NativePlugin& getSelf() const;

    /// @return True if the plugin is loaded successfully.
    bool load(ll::plugin::NativePlugin&);

    /// @return True if the plugin is enabled successfully.
    bool enable();

    /// @return True if the plugin is disabled successfully.
    bool disable();

private:
    ll::plugin::NativePlugin* mSelf{};
};

} // namespace LegacyAddonsManager
