#include "LegacyAddonsManager.h"
#include "ll/api/utils/StringUtils.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "nlohmann/json_fwd.hpp"

#include <Windows.h>
#include <ll/api/command/Command.h>
#include <ll/api/command/CommandHandle.h>
#include <ll/api/command/CommandRegistrar.h>
#include <ll/api/i18n/I18n.h>
#include <ll/api/io/FileUtils.h>
#include <ll/api/plugin/NativePlugin.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/utils/ErrorUtils.h>
#include <mc/deps/json/Json.h>
#include <mc/deps/json/JsonHelpers.h>
#include <mc/server/commands/CommandOrigin.h>
#include <mc/server/commands/CommandOutput.h>
#include <mc/server/common/PropertiesSettings.h>
#include <mc/world/level/Level.h>
#include <memory>
#include <nlohmann/json.hpp>

namespace LegacyAddonsManager {

#define VALID_ADDON_FILE_EXTENSION std::set<std::string>({".mcpack", ".mcaddon", ".zip"})
#define ZIP_PROGRAM_PATH           "./7za.exe"
#define ADDON_INSTALL_TEMP_DIR     "./plugins/LegacyAddonsManager/Temp/"
#define ADDON_INSTALL_MAX_WAIT     30000

std::pair<int, std::string> NewProcessSync(const std::string& process, int timeLimit = -1, bool noReadOutput = true) {
    SECURITY_ATTRIBUTES sa;
    HANDLE              hRead, hWrite;
    sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle       = TRUE;

    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return {-1, ""};
    STARTUPINFOW        si = {0};
    PROCESS_INFORMATION pi;

    si.cb = sizeof(STARTUPINFO);
    GetStartupInfoW(&si);
    si.hStdOutput = si.hStdError = hWrite;
    si.dwFlags                   = STARTF_USESTDHANDLES;

    auto wCmd = ll::string_utils::str2wstr(process);
    if (!CreateProcessW(
            nullptr,
            const_cast<wchar_t*>(wCmd.c_str()),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi
        )) {
        return {-1, ""};
    }
    CloseHandle(hWrite);
    CloseHandle(pi.hThread);

    if (timeLimit == -1) WaitForSingleObject(pi.hProcess, INFINITE);
    else {
        WaitForSingleObject(pi.hProcess, timeLimit);
        TerminateProcess(pi.hProcess, -1);
    }
    char        buffer[4096];
    std::string strOutput;
    DWORD       bytesRead, exitCode;

    GetExitCodeProcess(pi.hProcess, &exitCode);
    if (!noReadOutput) {
        while (true) {
            ZeroMemory(buffer, 4096);
            if (!ReadFile(hRead, buffer, 4096, &bytesRead, nullptr)) break;
            strOutput.append(buffer, bytesRead);
        }
    }
    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    return {exitCode, strOutput};
}

#define addonLogger LegacyAddonsManager::getInstance().getSelf().getLogger()
using ll::i18n_literals::operator""_tr;

std::vector<Addon> addons;

bool AutoInstallAddons(std::string path);


std::string GetLevelName() {
    if (ll::service::getPropertiesSettings().has_value()) {
        return ll::service::getPropertiesSettings()->getLevelName();
    } else {
        std::ifstream fin("server.properties");
        std::string   buf;
        while (getline(fin, buf)) {
            if (buf.find("level-name=") != std::string::npos) {
                if (buf.back() == '\n') buf.pop_back();
                if (buf.back() == '\r') buf.pop_back();
                return buf.substr(11);
            }
        }
    }
    return "";
}

// Helper
std::string GetAddonJsonFile(Addon::Type type) {
    std::string addonListFile = "./worlds/" + GetLevelName();
    switch (type) {
    case Addon::Type::BehaviorPack:
        return addonListFile + "/world_behavior_packs.json";
        break;
    case Addon::Type::ResourcePack:
        return addonListFile + "/world_resource_packs.json";
        break;
    default:
        break;
    }
    return "";
}

inline bool isManifestFile(std::string const& filename) {
    return filename == "manifest.json" || filename == "pack_manifest.json";
}

inline std::string FixMojangJson(std::string const& content) {
    Json::Value value;
    JsonHelpers::parseJson(content, value);
    return JsonHelpers::serialize(value);
}

std::optional<Addon> parseAddonFromPath(const std::filesystem::path& addonPath) {
    try {
        auto manifestPath = addonPath;
        manifestPath.append("manifest.json");
        if (!std::filesystem::exists(manifestPath)) {
            manifestPath = addonPath;
            manifestPath.append("pack_manifest.json");
        }
        auto manifestFile = ll::file_utils::readFile(ll::string_utils::u8str2str(manifestPath.u8string()));
        if (!manifestFile || manifestFile->empty()) throw std::exception("manifest.json not found!");
        std::string content = FixMojangJson(*manifestFile);

        auto  manifest = nlohmann::json::parse(content, nullptr, true, true);
        auto  header   = manifest["header"];
        auto  uuid     = header["uuid"];
        Addon addon;
        addon.name        = header["name"];
        addon.description = header["description"];
        addon.uuid        = uuid;
        addon.directory   = ll::string_utils::u8str2str(addonPath.u8string());

        auto ver      = header["version"];
        addon.version = ll::data::Version(ver[0], ver[1], ver[2]);

        std::string type = manifest["modules"][0]["type"];
        if (type == "resources") addon.type = Addon::Type::ResourcePack;
        else if (type == "data" || type == "script") addon.type = Addon::Type::BehaviorPack;
        else throw std::exception("Unknown type of addon pack!");

        return addon;
    } catch (const ll::error_utils::seh_exception& e) {
        addonLogger.error("Uncaught SEH Exception Detected!");
        addonLogger.error("In " __FUNCTION__ " " + ll::string_utils::u8str2str(addonPath.u8string()));
        addonLogger.error("Error: Code[{}] {}", e.code(), e.what());
    } catch (const std::exception& e) {
        addonLogger.error("Uncaught C++ Exception Detected!");
        addonLogger.error("In " __FUNCTION__ " " + ll::string_utils::u8str2str(addonPath.u8string()));
        addonLogger.error("Error: Code[{}] {}", -1, e.what());
    } catch (...) {
        addonLogger.error("Uncaught Exception Detected!");
        addonLogger.error("In " __FUNCTION__ " " + ll::string_utils::u8str2str(addonPath.u8string()));
    }
    return std::nullopt;
}

bool RemoveAddonFromList(Addon& addon) {
    auto jsonFile = GetAddonJsonFile(addon.type);
    if (!std::filesystem::exists(ll::string_utils::str2wstr(jsonFile))) {
        addonLogger.error("ll.addonsHelper.error.addonConfigNotFound"_tr());
        return false;
    }

    auto addonJsonContent = ll::file_utils::readFile(jsonFile);
    if (!addonJsonContent || addonJsonContent->empty()) {
        addonLogger.error("ll.addonsHelper.error.addonConfigNotFound"_tr());
        return false;
    }
    auto addonJson = nlohmann::json::parse(addonJsonContent.value(), nullptr, true, true);
    int  id        = 0;
    for (auto item : addonJson) {
        if (item["pack_id"] == addon.uuid) {
            addonJson.erase(id);
            bool res = ll::file_utils::writeFile(jsonFile, addonJson.dump(4));
            if (!res) {
                addonLogger.error("ll.addonsHelper.removeAddonFromList.fail"_tr(addon.name));
                return false;
            }
            addonLogger.info("ll.addonsHelper.removeAddonFromList.success"_tr(addon.name));
            return true;
        }
        ++id;
    }
    addonLogger.error("ll.addonsHelper.error.addonNotFound"_tr(addon.name));
    return false;
}

bool AddAddonToList(Addon& addon) {
    std::string addonListFile = GetAddonJsonFile(addon.type);
    if (!std::filesystem::exists(ll::string_utils::str2wstr(addonListFile))) {
        std::ofstream fout(addonListFile);
        fout << "[]" << std::flush;
    }

    try {

        bool exists = false;
        auto addonList =
            nlohmann::json::parse(ll::file_utils::readFile(addonListFile).value_or(""), nullptr, false, true);
        // Auto fix Addon List File
        if (!addonList.is_array()) {
            auto backupPath = ll::string_utils::u8str2str(
                                  std::filesystem::path(ll::string_utils::str2wstr(addonListFile)).stem().u8string()
                              )
                            + "_error.json";
            addonLogger.error("ll.addonsHelper.addAddonToList.invalidList"_tr(addonListFile, backupPath));
            std::error_code ec;
            std::filesystem::rename(
                ll::string_utils::str2wstr(addonListFile),
                std::filesystem::path(addonListFile).remove_filename().append(ll::string_utils::str2wstr(backupPath)),
                ec
            );
            addonList = "[]"_json;
        }
        for (auto& addonData : addonList) {
            if (addonData["pack_id"] == addon.uuid) {
                addonData["version"] = {addon.version.major, addon.version.minor, addon.version.patch};
                exists               = true;
                break;
            }
        }
        if (!exists) {
            auto newAddonData       = nlohmann::json::object();
            newAddonData["pack_id"] = addon.uuid;
            newAddonData["version"] = {addon.version.major, addon.version.minor, addon.version.patch};
            addonList.push_back(newAddonData);
        }
        bool res = ll::file_utils::writeFile(addonListFile, addonList.dump(4));
        if (!res) throw std::exception("Fail to write data back to addon list file!");
        addonLogger.info("ll.addonsHelper.addAddonToList.success"_tr(addon.name));
        return true;
    } catch (const std::exception& e) {
        addonLogger.error("ll.addonsHelper.addAddonToList.fail"_tr(addon.name, addonListFile));
        addonLogger.error("ll.addonsHelper.displayError"_tr(e.what()));
        addonLogger.error("ll.addonsHelper.error.installationAborted"_tr());
        return false;
    }
}
bool InstallAddonToLevel(const std::string& addonDir, const std::string& addonName) {
    auto addon = parseAddonFromPath(ll::string_utils::str2wstr(addonDir));
    if (!addon.has_value()) return false;
    std::string subPath;
    if (addon->type == Addon::Type::ResourcePack) subPath = "/resource_packs";
    else if (addon->type == Addon::Type::BehaviorPack) subPath = "/behavior_packs";


    // copy files
    std::string levelPath = "./worlds/" + GetLevelName();
    std::string toPath    = levelPath + subPath + "/" + addonName;

    // Avoid duplicate names or update addon if same uuid
    while (std::filesystem::exists(ll::string_utils::str2wstr(toPath))) {
        auto tmp = parseAddonFromPath(ll::string_utils::str2wstr(toPath));
        if (tmp.has_value() && tmp->uuid != addon->uuid) {
            toPath += "_";
        } else {
            std::error_code ec;
            std::filesystem::remove_all(ll::string_utils::str2wstr(toPath), ec);
            break;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(ll::string_utils::str2wstr(toPath), ec);
    std::filesystem::copy(
        ll::string_utils::str2wstr(addonDir),
        ll::string_utils::str2wstr(toPath),
        std::filesystem::copy_options::recursive,
        ec
    );

    // add addon to list file
    return AddAddonToList(*addon);
}

void FindManifest(std::vector<std::string>& result, const std::string& path) {
    std::filesystem::directory_iterator ent(ll::string_utils::str2wstr(path));

    bool foundManifest = false;
    for (auto& file : ent) {
        const auto& file_path = file.path();
        if (isManifestFile(ll::string_utils::u8str2str(file_path.filename().u8string()))) {
            result.push_back(ll::string_utils::u8str2str(std::filesystem::canonical(file_path).parent_path().u8string())
            );
            foundManifest = true;
            break;
        }
    }
    if (!foundManifest) {
        // No manifest file
        if (!AutoInstallAddons(path)) {
            std::filesystem::directory_iterator ent2(ll::string_utils::str2wstr(path));
            for (auto& file : ent2)
                if (file.is_directory()) FindManifest(result, ll::string_utils::u8str2str(file.path().u8string()));
        }
    }
}

bool AddonsManager::install(std::string packPath) {
    try {
        if (!std::filesystem::exists(ll::string_utils::str2wstr(packPath))) {
            addonLogger.error("ll.addonsHelper.error.addonFileNotFound"_tr(packPath));
            return false;
        }
        if (VALID_ADDON_FILE_EXTENSION.find(ll::string_utils::u8str2str(
                std::filesystem::path(ll::string_utils::str2wstr(packPath)).extension().u8string()
            ))
            == VALID_ADDON_FILE_EXTENSION.end()) {
            addonLogger.error("ll.addonsHelper.error.unsupportedFileType"_tr());
            return false;
        }

        std::string name = ll::string_utils::u8str2str(
            std::filesystem::path(ll::string_utils::str2wstr(packPath)).filename().u8string()
        );
        addonLogger.warn("ll.addonsHelper.install.installing"_tr(name));

        std::error_code ec;
        if (packPath.ends_with(".mcpack")) {
            std::string newPath = packPath;
            ll::string_utils::replaceAll(newPath, ".mcpack", ".zip");
            std::filesystem::rename(ll::string_utils::str2wstr(packPath), ll::string_utils::str2wstr(newPath), ec);
            packPath = newPath;
        }
        if (packPath.ends_with(".mcaddon")) {
            std::string newPath = packPath;
            ll::string_utils::replaceAll(newPath, ".mcaddon", ".zip");
            std::filesystem::rename(ll::string_utils::str2wstr(packPath), ll::string_utils::str2wstr(newPath), ec);
            packPath = newPath;
        }

        name = ll::string_utils::u8str2str(
            std::filesystem::path(ll::string_utils::str2wstr(packPath)).filename().u8string()
        );

        // filesystem::remove_all(ADDON_INSTALL_TEMP_DIR + name + "/", ec); //?
        // filesystem::create_directories(ADDON_INSTALL_TEMP_DIR + name + "/", ec);

        auto res = NewProcessSync(
            fmt::format(
                "{} x \"{}\" -o{} -aoa",
                ZIP_PROGRAM_PATH,
                packPath,
                "\"" ADDON_INSTALL_TEMP_DIR + name + "/\""
            ),
            ADDON_INSTALL_MAX_WAIT
        );
        if (res.first != 0) {
            addonLogger.error("ll.addonsHelper.install.error.failToUncompress.msg"_tr(name));
            addonLogger.error("ll.addonsHelper.install.error.failToUncompress.exitCode"_tr(res.first));
            addonLogger.error("ll.addonsHelper.install.error.failToUncompress.programOutput"_tr(res.second));
            addonLogger.error("ll.addonsHelper.error.installationAborted"_tr());
            std::filesystem::remove_all(ADDON_INSTALL_TEMP_DIR + name + "/", ec);
            return false;
        }

        std::vector<std::string> paths;
        FindManifest(paths, ADDON_INSTALL_TEMP_DIR + name + "/");

        for (auto& dir : paths) {
            std::string addonName =
                ll::string_utils::u8str2str(std::filesystem::path(ll::string_utils::str2wstr(dir)).filename().u8string()
                );
            if (addonName.empty() || addonName == "Temp")
                addonName = ll::string_utils::u8str2str(
                    std::filesystem::path(ll::string_utils::str2wstr(packPath)).stem().u8string()
                );
            if (!InstallAddonToLevel(dir, addonName)) throw std::exception("Error in Install Addon To Level ");
        }

        std::filesystem::remove_all(ADDON_INSTALL_TEMP_DIR + name + "/", ec);
        std::filesystem::remove_all(ll::string_utils::str2wstr(packPath), ec);
        return true;
    } catch (const ll::error_utils::seh_exception& e) {
        addonLogger.error("Uncaught SEH Exception Detected!");
        addonLogger.error("In " __FUNCTION__);
        addonLogger.error("Error: Code[{}] {}", e.code(), e.what());
    } catch (const std::exception& e) {
        addonLogger.error("Uncaught C++ Exception Detected!");
        addonLogger.error("In " __FUNCTION__);
        addonLogger.error("Error: Code[{}] {}", -1, e.what());
    } catch (...) {
        addonLogger.error("Uncaught Exception Detected!");
        addonLogger.error("In " __FUNCTION__);
    }
    return false;
}

bool AddonsManager::disable(std::string nameOrUuid) {
    try {
        auto addon = findAddon(nameOrUuid, true);
        if (!addon) return false;
        if (RemoveAddonFromList(*addon)) {
            addon->enable = false;
            return true;
        }

    } catch (...) {}
    return false;
}

bool AddonsManager::enable(std::string nameOrUuid) {
    try {
        auto addon = findAddon(nameOrUuid, true);
        if (!addon) return false;
        if (AddAddonToList(*addon)) {
            addon->enable = true;
            return true;
        }

    } catch (...) {}
    return false;
}

bool AddonsManager::uninstall(std::string nameOrUuid) {
    try {
        auto addon = findAddon(nameOrUuid, true);
        if (!addon) {
            addonLogger.error("ll.addonsHelper.error.addonNotFound"_tr());
            return false;
        }
        std::string addonName = addon->name;
        RemoveAddonFromList(*addon);
        std::error_code ec;
        std::filesystem::remove_all(ll::string_utils::str2wstr(addon->directory), ec);
        for (auto i = addons.begin(); i != addons.end(); ++i)
            if (i->uuid == addon->uuid) {
                addons.erase(i);
                break;
            }
        addonLogger.info("ll.addonsHelper.uninstall.success"_tr(addonName));
    } catch (...) {}
    return false;
}

std::vector<Addon*> AddonsManager::getAllAddons() {
    std::vector<Addon*> res;
    for (auto& addon : addons) res.push_back(&addon);
    return res;
}

Addon* AddonsManager::findAddon(const std::string& nameOrUuid, bool fuzzy) {
    Addon* possible   = nullptr;
    bool   multiMatch = false;
    for (auto& addon : addons) {
        if (addon.uuid == nameOrUuid) return &addon;
        std::string addonName  = addon.name;
        std::string targetName = nameOrUuid;
        if (ll::string_utils::removeEscapeCode(addonName) == ll::string_utils::removeEscapeCode(targetName))
            return &addon;
        if (!fuzzy) continue;
        // Simple fuzzy matching
        std::transform(addonName.begin(), addonName.end(), addonName.begin(), ::tolower);
        std::transform(targetName.begin(), targetName.end(), targetName.begin(), ::tolower);
        if (addonName.starts_with(targetName)) {
            if (possible) multiMatch = true;
            else possible = &addon;
        }
    }
    if (multiMatch) return nullptr;
    else return possible;
}

void FindAddons(std::string jsonPath, std::string packsDir) {
    namespace fs = std::filesystem;
    try {
        if (!fs::exists(ll::string_utils::str2wstr(jsonPath)) && !fs::exists(ll::string_utils::str2wstr(packsDir)))
            return;
        if (!fs::exists(ll::string_utils::str2wstr(jsonPath))) ll::file_utils::writeFile(jsonPath, "[]");
        if (!fs::exists(ll::string_utils::str2wstr(packsDir)))
            fs::create_directories(ll::string_utils::str2wstr(packsDir));

        auto content = ll::file_utils::readFile(jsonPath);
        if (!content || content->empty()) {
            ll::file_utils::writeFile(jsonPath, "[]");
            content = "[]";
        }
        std::set<std::string> validPackIDs;
        try {
            auto addonList = nlohmann::json::parse(content.value(), nullptr, true, true);
            for (auto addon : addonList) {
                std::string pktid = addon["pack_id"];
                validPackIDs.insert(pktid);
            }
        } catch (const std::exception&) {
            addonLogger.error("ll.addonsHelper.error.parsingEnabledAddonsList"_tr());
        }

        std::filesystem::directory_iterator ent(ll::string_utils::str2wstr(packsDir));
        for (auto& dir : ent) {
            if (!dir.is_directory()) continue;
            auto addon = parseAddonFromPath(dir);
            if (!addon) continue;
            if (validPackIDs.find(addon->uuid) != validPackIDs.end()) addon->enable = true;
            addons.emplace_back(std::move(*addon));
        }
    } catch (...) {
        return;
    }
}

void BuildAddonsList() {
    std::string levelPath = "./worlds/" + GetLevelName();

    FindAddons(levelPath + "/world_behavior_packs.json", levelPath + "/behavior_packs");
    FindAddons(levelPath + "/world_resource_packs.json", levelPath + "/resource_packs");

    std::sort(addons.begin(), addons.end(), [](Addon const& _Left, Addon const& _Right) {
        if (_Left.enable && !_Right.enable) return true;
        if (_Left.type == Addon::Type::ResourcePack && _Right.type == Addon::Type::BehaviorPack) return true;
        return false;
    });
}

bool AutoInstallAddons(std::string path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(ll::string_utils::str2wstr(path))) {
        fs::create_directories(ll::string_utils::str2wstr(path), ec);
        addonLogger.info("ll.addonsHelper.autoInstall.tip.dirCreated"_tr("./plugins/LegacyAddonsManager/addons"));
        return false;
    }
    std::vector<std::string> toInstallList;

    fs::directory_iterator ent(ll::string_utils::str2wstr(path));
    for (auto& file : ent) {
        if (!file.is_regular_file()) continue;

        if (VALID_ADDON_FILE_EXTENSION.count(ll::string_utils::u8str2str(file.path().extension().u8string())) > 0) {
            toInstallList.push_back(ll::string_utils::u8str2str(file.path().lexically_normal().u8string()));
        }
    }

    if (toInstallList.empty()) return false;

    addonLogger.info("ll.addonsHelper.autoInstall.working"_tr(toInstallList.size()));
    int cnt = 0;
    for (auto& addonPath : toInstallList) {
        addonLogger.debug("Installing \"{}\"...", addonPath);
        if (!AddonsManager::install(addonPath)) {
            // filesystem::remove_all(ADDON_INSTALL_TEMP_DIR, ec);
            break;
        } else {
            ++cnt;
            addonLogger.info("ll.addonsHelper.autoInstall.installed"_tr(addonPath));
        }
    }

    if (cnt == 0) {
        addonLogger.error("ll.addonsHelper.error.noAddonInstalled"_tr());
    } else {
        addonLogger.info("ll.addonsHelper.autoInstall.installedCount"_tr(cnt));
    }
    return true;
}

enum AddonsOperation { enable, disable, uninstall, remove };

struct AddonsCommand {
    AddonsOperation operation;
    std::string     name;
    int             index;
};

void RegisterCommand() {
    auto& command = ll::command::CommandRegistrar::getInstance().getOrCreateCommand(
        "addons",
        "Addons manager's main commmand  (Restart required after addon changes)",
        CommandPermissionLevel::Host
    );
    command.overload<AddonsCommand>()
        .required("operation")
        .required("name")
        .execute<[](CommandOrigin const&, CommandOutput& output, AddonsCommand const& commandContent) {
            switch (commandContent.operation) {
            case AddonsOperation::enable: {
                auto addon = AddonsManager::findAddon(commandContent.name, true);
                if (addon) {
                    if (AddonsManager::enable(addon->uuid)) {
                        output.success();
                    }
                } else {
                    output.error("ll.addonsHelper.error.addonNotfound"_tr(commandContent.name));
                }
                break;
            }
            case AddonsOperation::disable: {
                auto addon = AddonsManager::findAddon(commandContent.name, true);
                if (addon) {
                    if (AddonsManager::disable(addon->uuid)) {
                        output.success();
                    }
                } else {
                    output.error("ll.addonsHelper.error.addonNotfound"_tr(commandContent.name));
                }
                break;
            }
            case AddonsOperation::remove:
            case AddonsOperation::uninstall: {
                auto addon = AddonsManager::findAddon(commandContent.name, true);
                if (addon) {
                    if (AddonsManager::uninstall(addon->uuid)) {
                        output.success();
                    }
                } else {
                    output.error("ll.addonsHelper.error.addonNotfound"_tr(commandContent.name));
                }
                break;
            }
            }
        }>();
    command.overload<AddonsCommand>()
        .required("operation")
        .required("index")
        .execute<[](CommandOrigin const&, CommandOutput& output, AddonsCommand const& commandContent) {
            switch (commandContent.operation) {
            case AddonsOperation::enable: {
                auto allAddons = AddonsManager::getAllAddons();
                if (commandContent.index - 1 >= 0 && commandContent.index - 1 < static_cast<int>(allAddons.size())) {
                    if (AddonsManager::enable(allAddons[commandContent.index - 1]->uuid)) {
                        output.success();
                    }
                } else {
                    output.error("ll.addonsHelper.error.outOfRange"_tr(commandContent.index));
                }
                break;
            }
            case AddonsOperation::disable: {
                auto allAddons = AddonsManager::getAllAddons();
                if (commandContent.index - 1 >= 0 && commandContent.index - 1 < static_cast<int>(allAddons.size())) {
                    if (AddonsManager::disable(allAddons[commandContent.index - 1]->uuid)) {
                        output.success();
                    }
                } else {
                    output.error("ll.addonsHelper.error.outOfRange"_tr(commandContent.index));
                }
                break;
            }
            case AddonsOperation::remove:
            case AddonsOperation::uninstall: {
                auto allAddons = AddonsManager::getAllAddons();
                if (commandContent.index - 1 >= 0 && commandContent.index - 1 < static_cast<int>(allAddons.size())) {
                    if (AddonsManager::uninstall(allAddons[commandContent.index - 1]->uuid)) {
                        output.success();
                    }
                } else {
                    output.error("ll.addonsHelper.error.outOfRange"_tr(commandContent.index));
                }
                break;
            }
            }
        }>();
    command.overload<AddonsCommand>()
        .text("list")
        .optional("name")
        .execute<[](CommandOrigin const&, CommandOutput& output, AddonsCommand const& commandContent) {
            if (!commandContent.name.empty()) {
                auto addon = AddonsManager::findAddon(commandContent.name, true);
                if (addon) {
                    std::ostringstream oss;
                    oss << "Addon <" << addon->name << "§r>" << (addon->enable ? " §aEnabled" : " §cDisabled")
                        << "\n\n";
                    oss << "- §aName§r:  " << addon->name << "\n";
                    oss << "- §aUUID§r:  " << addon->uuid << "\n";
                    oss << "- §aDescription§r:  " << addon->description << "\n";
                    oss << "- §aVersion§r:  v" << addon->version.to_string() << "\n";
                    oss << "- §aType§r:  " << magic_enum::enum_name(addon->type) << "\n";
                    oss << "- §aDirectory§r:  " << addon->directory << "\n";
                    output.success(oss.str());
                } else {
                    output.error("ll.addonsHelper.error.addonNotfound"_tr(commandContent.name));
                }
            } else {
                if (addons.empty()) {
                    output.error("ll.addonsHelper.error.noAddonInstalled"_tr());
                    return;
                }

                output.success("ll.addonsHelper.cmd.output.list.overview"_tr(addons.size()));
                for (auto index = 0; index < addons.size(); ++index) {
                    auto&       addon     = addons[index];
                    std::string addonName = addon.name;
                    if (addonName.find("§") == std::string::npos) addonName = "§b" + addonName;
                    std::string desc = addon.description;
                    if (desc.find("§") == std::string::npos) desc = "§7" + desc;

                    std::string addonType = (addon.type == Addon::Type::ResourcePack ? "ResourcePack" : "BehaviorPack");
                    if (addon.enable) {
                        output.success(fmt::format(
                            "§e{:>2}§r: {} §a[v{}] §8({})",
                            index + 1,
                            addonName,
                            addon.version.to_string(),
                            addonType
                        ));
                        output.success(fmt::format("    {}", desc));
                    } else {
                        output.success(fmt::format(
                            "§e{:>2}§r: §8{} [v{}] ({})",
                            index + 1,
                            ll::string_utils::removeEscapeCode(addonName),
                            addon.version.to_string(),
                            addonType
                        ));
                        output.success(fmt::format("    §8Disabled"));
                    }
                }
            }
        }>();
    command.overload<AddonsCommand>()
        .text("list")
        .required("index")
        .execute<[](CommandOrigin const&, CommandOutput& output, AddonsCommand const& commandContent) {
            auto allAddons = AddonsManager::getAllAddons();
            if (commandContent.index - 1 >= 0 && commandContent.index - 1 < static_cast<int>(allAddons.size())) {
                Addon*             addon = allAddons[commandContent.index - 1];
                std::ostringstream oss;
                oss << "Addon <" << addon->name << "§r>" << (addon->enable ? " §aEnabled" : " §cDisabled") << "\n\n";
                oss << "- §aName§r:  " << addon->name << "\n";
                oss << "- §aUUID§r:  " << addon->uuid << "\n";
                oss << "- §aDescription§r:  " << addon->description << "\n";
                oss << "- §aVersion§r:  v" << addon->version.to_string() << "\n";
                oss << "- §aType§r:  " << magic_enum::enum_name(addon->type) << "\n";
                oss << "- §aDirectory§r:  " << addon->directory << "\n";
                output.success(oss.str());
            } else {
                output.error("ll.addonsHelper.error.outOfRange"_tr(commandContent.index));
            }
        }>();
    command.overload<AddonsCommand>()
        .text("install")
        .required("name")
        .execute<[](CommandOrigin const&, CommandOutput& output, AddonsCommand const& commandContent) {
            if (AddonsManager::install(commandContent.name)) {
                std::filesystem::remove_all(ADDON_INSTALL_TEMP_DIR);
                output.success();
            } else {
                output.error("Failed to install addon {0}"_tr(commandContent.name));
            }
        }>();
}

void InitAddonsHelper() {
    namespace fs = std::filesystem;

    fs::remove_all(ADDON_INSTALL_TEMP_DIR);
    fs::create_directories(ADDON_INSTALL_TEMP_DIR);

    AutoInstallAddons("./plugins/LegacyAddonsManager/addons");
    BuildAddonsList();

    fs::remove_all(ADDON_INSTALL_TEMP_DIR);
}


LegacyAddonsManager::LegacyAddonsManager() = default;

LegacyAddonsManager& LegacyAddonsManager::getInstance() {
    static LegacyAddonsManager instance;
    return instance;
}

ll::plugin::NativePlugin& LegacyAddonsManager::getSelf() const { return *mSelf; }

bool LegacyAddonsManager::load(ll::plugin::NativePlugin& self) {
    mSelf = std::addressof(self);
    getSelf().getLogger().info("loading...");
    ll::i18n::load("./plugins/LegacyAddonsManager/lang/");
    InitAddonsHelper();
    return true;
}

bool LegacyAddonsManager::enable() {
    RegisterCommand();
    return true;
}

bool LegacyAddonsManager::disable() { return true; }

extern "C" {
_declspec(dllexport) bool ll_plugin_load(ll::plugin::NativePlugin& self) {
    return LegacyAddonsManager::getInstance().load(self);
}

_declspec(dllexport) bool ll_plugin_enable(ll::plugin::NativePlugin&) {
    return LegacyAddonsManager::getInstance().enable();
}

_declspec(dllexport) bool ll_plugin_disable(ll::plugin::NativePlugin&) {
    return LegacyAddonsManager::getInstance().disable();
}

/// @warning Unloading the plugin may cause a crash if the plugin has not released all of its
/// resources. If you are unsure, keep this function commented out.
// _declspec(dllexport) bool ll_plugin_unload(ll::plugin::NativePlugin&) {
//     return LegacyAddonsManager::getInstance().unload();
// }
}

} // namespace LegacyAddonsManager
