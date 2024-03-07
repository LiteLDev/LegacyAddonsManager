#pragma once

#include <ll/api/plugin/NativePlugin.h>

namespace AddonsHelper {

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

class AddonsHelper {
    AddonsHelper();

public:
    AddonsHelper(AddonsHelper&&)                 = delete;
    AddonsHelper(const AddonsHelper&)            = delete;
    AddonsHelper& operator=(AddonsHelper&&)      = delete;
    AddonsHelper& operator=(const AddonsHelper&) = delete;

    static AddonsHelper& getInstance();

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

} // namespace AddonsHelper
