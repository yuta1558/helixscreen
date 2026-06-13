// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
#include "system/telemetry_manager.h"
#define CONFIG_RECORD_ERROR(...) TelemetryManager::instance().record_error(__VA_ARGS__)
#else
#define CONFIG_RECORD_ERROR(...) ((void)0)
#endif
#include "ui_error_reporting.h"

#include "app_constants.h"
#include "config_backup.h"
#include "config_testing.h"
#include "data_root_resolver.h"
#include "platform_capabilities.h"
#include "printer_detector.h"
#include "runtime_config.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sys/stat.h>
// C++17 filesystem - use std::filesystem if available, fall back to experimental
#if __cplusplus >= 201703L && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

using namespace helix;

using AppConstants::Update::config_backup_fallback;
using AppConstants::Update::CONFIG_BACKUP_PRIMARY;
using AppConstants::Update::env_backup_fallback;
using AppConstants::Update::ENV_BACKUP_PRIMARY;
using AppConstants::Update::legacy_config_backup_fallback;
using AppConstants::Update::LEGACY_CONFIG_BACKUP_PRIMARY;

Config* Config::instance{NULL};

namespace {

/// Default macro configuration - shared between init() and reset_to_defaults()
json get_default_macros() {
    return {{"load_filament", {{"label", "Load"}, {"gcode", "LOAD_FILAMENT"}}},
            {"unload_filament", {{"label", "Unload"}, {"gcode", "UNLOAD_FILAMENT"}}},
            {"macro_1", {{"label", "Clean Nozzle"}, {"gcode", "HELIX_CLEAN_NOZZLE"}}},
            {"macro_2", {{"label", "Bed Level"}, {"gcode", "HELIX_BED_MESH_IF_NEEDED"}}},
            {"cooldown", "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\nSET_HEATER_TEMPERATURE "
                         "HEATER=heater_bed TARGET=0"}};
}

/// Default printer configuration - shared between init() and reset_to_defaults()
/// @param moonraker_host Host address (empty string for reset, "127.0.0.1" for new config)
json get_default_printer_config(const std::string& moonraker_host) {
    return {
        {"moonraker_api_key", false},
        {"moonraker_host", moonraker_host},
        {"moonraker_port", 7125},
        {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
        {"temp_sensors", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
        {"fans",
         {{"part", "fan"}, {"hotend", "heater_fan hotend_fan"}, {"chamber", ""}, {"exhaust", ""}}},
        {"leds",
         {{"strip", ""}, {"selected", json::array()}}}, // Empty default - wizard will auto-detect
        {"extra_sensors", json::object()},
        {"hardware",
         {{"optional", json::array()},
          {"expected", json::array()},
          {"last_snapshot", json::object()}}},
        {"default_macros", get_default_macros()}};
}

/// Default display configuration section
/// Used for both new configs and ensuring display section exists with defaults
json get_default_display_config() {
    return {{"sleep_sec", 1200}, {"dim_sec", 600},         {"dim_brightness", 30},
            {"drm_device", ""},  {"gcode_render_mode", 0}, {"bed_mesh_render_mode", 0}};
}

/// Migrate legacy display settings from root level to /display/ section
/// @param data JSON config data to migrate (modified in place)
/// @return true if migration occurred, false if no migration needed
bool migrate_display_config(json& data) {
    // Check for root-level display_rotate as indicator of old format
    if (!data.contains("display_rotate")) {
        return false; // Already migrated or new config
    }

    spdlog::info("[Config] Migrating display settings to /display/ section");

    // Ensure display section exists
    if (!data.contains("display")) {
        data["display"] = json::object();
    }

    // Migrate root-level display settings (only if target key doesn't already exist)
    if (data.contains("display_rotate")) {
        if (!data["display"].contains("rotate")) {
            data["display"]["rotate"] = data["display_rotate"];
            spdlog::info("[Config] Migrated display_rotate -> /display/rotate");
        }
        data.erase("display_rotate");
    }

    if (data.contains("display_sleep_sec")) {
        if (!data["display"].contains("sleep_sec")) {
            data["display"]["sleep_sec"] = data["display_sleep_sec"];
            spdlog::info("[Config] Migrated display_sleep_sec -> /display/sleep_sec");
        }
        data.erase("display_sleep_sec");
    }

    if (data.contains("display_dim_sec")) {
        if (!data["display"].contains("dim_sec")) {
            data["display"]["dim_sec"] = data["display_dim_sec"];
            spdlog::info("[Config] Migrated display_dim_sec -> /display/dim_sec");
        }
        data.erase("display_dim_sec");
    }

    if (data.contains("display_dim_brightness")) {
        if (!data["display"].contains("dim_brightness")) {
            data["display"]["dim_brightness"] = data["display_dim_brightness"];
            spdlog::info("[Config] Migrated display_dim_brightness -> /display/dim_brightness");
        }
        data.erase("display_dim_brightness");
    }

    // Migrate touch calibration settings (only if target keys don't already exist)
    if (data.contains("touch_calibrated") || data.contains("touch_calibration")) {
        // Ensure calibration subsection exists
        if (!data["display"].contains("calibration")) {
            data["display"]["calibration"] = json::object();
        }

        if (data.contains("touch_calibrated")) {
            if (!data["display"]["calibration"].contains("valid")) {
                data["display"]["calibration"]["valid"] = data["touch_calibrated"];
                spdlog::info("[Config] Migrated touch_calibrated -> /display/calibration/valid");
            }
            data.erase("touch_calibrated");
        }

        if (data.contains("touch_calibration")) {
            const auto& cal = data["touch_calibration"];
            for (const auto& key : {"a", "b", "c", "d", "e", "f"}) {
                if (cal.contains(key) && !data["display"]["calibration"].contains(key)) {
                    data["display"]["calibration"][key] = cal[key];
                }
            }
            data.erase("touch_calibration");
            spdlog::info(
                "[Config] Migrated touch_calibration/{{a-f}} -> /display/calibration/{{a-f}}");
        }
    }

    spdlog::info("[Config] Display settings migration complete");
    return true;
}

/// Erase a value at a JSON pointer path without triggering deprecated
/// json_pointer implicit string conversion (nlohmann json 3.11+ deprecation)
void erase_at_pointer(json& data, const json::json_pointer& ptr) {
    // Navigate to parent, then erase the leaf key
    auto ptr_str = ptr.to_string();
    auto last_slash = ptr_str.rfind('/');
    if (last_slash == 0) {
        // Top-level key like "/foo"
        data.erase(ptr_str.substr(1));
    } else if (last_slash != std::string::npos) {
        // Nested key like "/foo/bar" — get parent, erase leaf
        json::json_pointer parent_ptr(ptr_str.substr(0, last_slash));
        data[parent_ptr].erase(ptr_str.substr(last_slash + 1));
    }
}

/// Migrate config keys from old paths to new paths
/// @param data JSON config data to migrate (modified in place)
/// @param migrations Vector of {from_path, to_path} pairs (JSON pointer format)
/// @return true if any migration occurred, false if no migration needed
bool migrate_config_keys(json& data,
                         const std::vector<std::pair<std::string, std::string>>& migrations) {
    bool any_migrated = false;

    for (const auto& [from_path, to_path] : migrations) {
        json::json_pointer from_ptr(from_path);
        json::json_pointer to_ptr(to_path);

        // Skip if source doesn't exist
        if (!data.contains(from_ptr)) {
            continue;
        }

        // Skip if target already exists (don't overwrite)
        if (data.contains(to_ptr)) {
            spdlog::debug("[Config] Migration skipped: {} already exists", to_path);
            erase_at_pointer(data, from_ptr);
            any_migrated = true;
            continue;
        }

        // Ensure parent path exists for target
        // For example, if to_path is "/input/calibration", ensure "/input" exists
        auto last_slash = to_path.rfind('/');
        if (last_slash != std::string::npos && last_slash > 0) {
            std::string parent_path = to_path.substr(0, last_slash);
            json::json_pointer parent_ptr(parent_path);
            if (!data.contains(parent_ptr)) {
                data[parent_ptr] = json::object();
            }
        }

        // Copy value to new location and remove from old
        data[to_ptr] = data[from_ptr];
        erase_at_pointer(data, from_ptr);
        spdlog::info("[Config] Migrated {} -> {}", from_path, to_path);
        any_migrated = true;
    }

    return any_migrated;
}

// ============================================================================
// Versioned config migrations
// ============================================================================

} // end anonymous namespace

// Test injection seam: allows unit tests to force a platform tier classification
// without monkey-patching /proc. Production code never sets this.
namespace {
static std::optional<helix::PlatformTier> g_forced_tier_for_migration;
} // namespace

namespace helix::config_testing {
void set_forced_tier_for_migration(std::optional<helix::PlatformTier> tier) {
    g_forced_tier_for_migration = tier;
}
} // namespace helix::config_testing

namespace {

#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
static helix::PlatformTier current_tier_for_migration() {
    if (g_forced_tier_for_migration.has_value()) {
        return *g_forced_tier_for_migration;
    }
    return helix::PlatformCapabilities::detect().tier;
}
#endif

/// Migration v0→v1: Sound support added — default sounds OFF for existing configs.
/// Before sound actually worked, configs had sounds_enabled: true as a harmless default.
/// Force it off so upgrading users don't get surprise beeps.
static void migrate_v0_to_v1(json& config) {
    if (config.contains("sounds_enabled")) {
        config["sounds_enabled"] = false;
        spdlog::info("[Config] Migration v1: disabled sounds_enabled for existing config");
    }
}

/// Migration v1→v2: Multi-LED support — convert single LED string to array
static void migrate_v1_to_v2(json& config) {
    json::json_pointer strip_ptr("/printer/leds/strip");
    json::json_pointer selected_ptr("/printer/leds/selected");

    // If new array path already exists, nothing to do
    if (config.contains(selected_ptr)) {
        return;
    }

    // Convert old single string to array
    if (config.contains(strip_ptr)) {
        auto& strip_val = config[strip_ptr];
        if (strip_val.is_string()) {
            std::string led = strip_val.get<std::string>();
            if (!led.empty()) {
                config[selected_ptr] = json::array({led});
                spdlog::info("[Config] Migration v2: converted LED '{}' from /printer/leds/strip "
                             "to /printer/leds/selected array",
                             led);
            } else {
                config[selected_ptr] = json::array();
                spdlog::info(
                    "[Config] Migration v2: empty LED strip, created empty selected array");
            }
        }
        // Don't remove /printer/leds/strip - keep for wizard backward compat
    } else {
        // No LED configured at all - create empty array
        config[selected_ptr] = json::array();
        spdlog::info("[Config] Migration v2: no LED configured, created empty selected array");
    }
}

/// Migration v2→v3: Reset jitter_threshold from 15 to 0 (disabled by default).
/// The jitter filter competed with LVGL's scroll_limit, adding perceptible drag delay.
/// Users with genuinely noisy panels can re-enable via config or HELIX_TOUCH_JITTER env.
static void migrate_v2_to_v3(json& config) {
    json::json_pointer ptr("/input/jitter_threshold");
    if (config.contains(ptr) && config[ptr].is_number_integer() && config[ptr].get<int>() == 15) {
        config[ptr] = 5;
        spdlog::info("[Config] Migration v3: reset jitter_threshold 15 -> 5");
    }
}

/// Migration v3→v4: Restructure single /printer to multi-printer /printers map.
/// Moves the old singular "printer" object under "printers/{slug}/" and sets active_printer_id.
/// Also moves root-level "filament", "panel_widgets" under the printer entry.
static void migrate_v3_to_v4(json& config) {
    // Skip if already has /printers (idempotent)
    if (config.contains("printers")) {
        return;
    }

    // Skip if no old /printer section exists
    if (!config.contains("printer") || !config["printer"].is_object()) {
        return;
    }

    json printer_data = config["printer"];

    // Determine the slug ID from the printer name
    std::string printer_name;
    if (printer_data.contains("printer_name") && printer_data["printer_name"].is_string()) {
        printer_name = printer_data["printer_name"].get<std::string>();
    } else if (printer_data.contains("name") && printer_data["name"].is_string()) {
        printer_name = printer_data["name"].get<std::string>();
    }
    std::string slug = printer_name.empty() ? "default" : Config::slugify(printer_name);
    if (slug.empty()) {
        slug = "default";
    }

    // Move root-level per-printer sections into the printer entry
    if (config.contains("filament") && config["filament"].is_object()) {
        printer_data["filament"] = config["filament"];
        config.erase("filament");
    }
    if (config.contains("panel_widgets") && config["panel_widgets"].is_object()) {
        printer_data["panel_widgets"] = config["panel_widgets"];
        config.erase("panel_widgets");
    }

    // Copy root-level wizard_completed into the printer entry
    if (config.contains("wizard_completed")) {
        printer_data["wizard_completed"] = config["wizard_completed"];
        // Keep root-level for backward compatibility
    }

    // Create the new printers map and set the active printer
    config["printers"] = {{slug, printer_data}};
    config["active_printer_id"] = slug;

    // Remove the old singular "printer" key
    config.erase("printer");

    // Migrate /display/printer_image to per-printer /printers/{id}/printer_image
    if (config.contains("display") && config["display"].contains("printer_image")) {
        config["printers"][slug]["printer_image"] = config["display"]["printer_image"];
        config["display"].erase("printer_image");
        spdlog::info(
            "[Config] Migration v4: moved /display/printer_image to /printers/{}/printer_image",
            slug);
    }

    spdlog::info("[Config] Migration v4: restructured /printer to /printers/{}", slug);
}

/// Default show_printer_switcher to false for single-printer configs.
/// Shared by v4→v5 and v5→v6 migrations (v6 re-runs for fresh v5 installs that had wrong default).
static void default_printer_switcher_off(json& config, int target_version) {
    if (config.contains("/printers/show_printer_switcher"_json_pointer)) {
        return;
    }

    int printer_count = 0;
    if (config.contains("printers") && config["printers"].is_object()) {
        for (auto& [key, val] : config["printers"].items()) {
            if (val.is_object()) {
                printer_count++;
            }
        }
    }

    if (printer_count <= 1) {
        config["/printers/show_printer_switcher"_json_pointer] = false;
        spdlog::info(
            "[Config] Migration v{}: disabled show_printer_switcher for single-printer config",
            target_version);
    }
}

static void migrate_v4_to_v5(json& config) {
    default_printer_switcher_off(config, 5);
}
static void migrate_v5_to_v6(json& config) {
    default_printer_switcher_off(config, 6);
}

/// Bump default brightness from 50% to 80% for users who never changed it.
static void migrate_v6_to_v7(json& config) {
    if (config.contains("brightness") && config["brightness"].is_number() &&
        config["brightness"].get<int>() == 50) {
        config["brightness"] = 80;
        spdlog::info("[Config] Migration v7: updated default brightness from 50% to 80%");
    }
}

/// Remap toolhead_style after alphabetical reorder of enum values.
/// Old: AUTO=0, DEFAULT=1, STEALTHBURNER=2, A4T=3, JABBERWOCKY=4
/// New: AUTO=0, DEFAULT=1, A4T=2, ANTHEAD=3, JABBERWOCKY=4, STEALTHBURNER=5
static void migrate_v7_to_v8(json& config) {
    auto remap_toolhead = [](json& printers_obj) {
        for (auto& [id, printer] : printers_obj.items()) {
            json::json_pointer ptr("/appearance/toolhead_style");
            if (printer.contains(ptr) && printer[ptr].is_number_integer()) {
                int old_val = printer[ptr].get<int>();
                // Only 2 (old STEALTHBURNER) and 3 (old A4T) need remapping
                if (old_val == 2) {
                    printer[ptr] = 5; // STEALTHBURNER
                    spdlog::info(
                        "[Config] Migration v8: remapped toolhead_style 2→5 (Stealthburner) "
                        "for printer {}",
                        id);
                } else if (old_val == 3) {
                    printer[ptr] = 2; // A4T
                    spdlog::info("[Config] Migration v8: remapped toolhead_style 3→2 (A4T) "
                                 "for printer {}",
                                 id);
                }
            }
        }
    };

    if (config.contains("printers") && config["printers"].is_object()) {
        remap_toolhead(config["printers"]);
    }
}

/// Re-apply brightness 50->80 bump for users whose config was written with the
/// old default of 50 after v7 migration already ran (the default in
/// get_default_config() was still 50, so new installs after v7 got 50 again).
static void migrate_v8_to_v9(json& config) {
    if (config.contains("brightness") && config["brightness"].is_number() &&
        config["brightness"].get<int>() == 50) {
        config["brightness"] = 80;
        spdlog::info("[Config] Migration v9: updated default brightness from 50% to 80%");
    }
}

/// Consolidate "power" widget into "power_device" with __all__ sentinel.
/// Scans all printers/panels/pages for widget entries with id=="power" and replaces
/// them with "power_device:N" (using the next available instance number).
static void migrate_v9_to_v10(json& config) {
    if (!config.contains("printers"))
        return;

    for (auto& [printer_id, printer] : config["printers"].items()) {
        if (!printer.is_object() || !printer.contains("panel_widgets"))
            continue;

        for (auto& [panel_id, panel] : printer["panel_widgets"].items()) {
            if (!panel.is_object() || !panel.contains("pages") || !panel["pages"].is_array())
                continue;

            // Find max power_device instance number across all pages
            int max_instance = 0;
            for (auto& page : panel["pages"]) {
                if (!page.contains("widgets") || !page["widgets"].is_array())
                    continue;
                for (auto& widget : page["widgets"]) {
                    std::string id = widget.value("id", "");
                    if (id.substr(0, 13) == "power_device:") {
                        try {
                            int n = std::stoi(id.substr(13));
                            if (n > max_instance)
                                max_instance = n;
                        } catch (...) {
                        }
                    }
                }
            }

            // Migrate "power" → "power_device:N+1"
            for (auto& page : panel["pages"]) {
                if (!page.contains("widgets") || !page["widgets"].is_array())
                    continue;
                for (auto& widget : page["widgets"]) {
                    if (widget.value("id", "") == "power") {
                        max_instance++;
                        widget["id"] = "power_device:" + std::to_string(max_instance);
                        widget["config"] = {{"device", "__all__"}, {"icon", "power_cycle"}};
                        spdlog::info("[Config] Migrated power widget to power_device:{} for "
                                     "printer '{}'",
                                     max_instance, printer_id);
                    }
                }
            }
        }
    }
}

/// Migrate PID heat rates to shared thermal path; strip heating phases from predictor entries.
static void migrate_v10_to_v11(json& config) {
    // 1. Move PID heat rates to shared thermal path
    for (const auto& heater : {"extruder", "heater_bed"}) {
        if (config.contains("calibration") && config["calibration"].contains("pid_history") &&
            config["calibration"]["pid_history"].contains(heater) &&
            config["calibration"]["pid_history"][heater].contains("heat_rate")) {
            // Copy to new location if not already present
            bool dest_exists = config.contains("thermal") && config["thermal"].contains("rates") &&
                               config["thermal"]["rates"].contains(heater) &&
                               config["thermal"]["rates"][heater].contains("heat_rate");

            if (!dest_exists) {
                config["thermal"]["rates"][heater]["heat_rate"] =
                    config["calibration"]["pid_history"][heater]["heat_rate"];
                spdlog::info("[Config] Migration v11: copied heat_rate for '{}' to /thermal/rates",
                             heater);
            }

            // Erase from old location (leave parent intact for oscillation_duration)
            config["calibration"]["pid_history"][heater].erase("heat_rate");
            spdlog::info(
                "[Config] Migration v11: removed heat_rate from /calibration/pid_history/{}",
                heater);
        }
    }

    // 2. Strip heating phases (HEATING_BED=3, HEATING_NOZZLE=4) from predictor entries
    if (config.contains("print_start_history") &&
        config["print_start_history"].contains("entries") &&
        config["print_start_history"]["entries"].is_array()) {
        int count = 0;
        for (auto& entry : config["print_start_history"]["entries"]) {
            if (entry.contains("phases") && entry["phases"].is_object()) {
                entry["phases"].erase("3"); // HEATING_BED
                entry["phases"].erase("4"); // HEATING_NOZZLE
                count++;
            }
        }
        if (count > 0) {
            spdlog::info(
                "[Config] Migration v11: stripped heating phases from {} predictor entries", count);
        }
    }
}

/// Consolidate chamber assignment keys: move `printer/chamber_sensor` → `temp_sensors/chamber`
/// and `printer/chamber_heater` → `heaters/chamber` under each printer, matching the flat
/// per-hardware-type convention used everywhere else (heaters/bed, fans/chamber, etc.).
static void migrate_v11_to_v12(json& config) {
    if (!config.contains("printers") || !config["printers"].is_object())
        return;

    for (auto& [printer_id, printer] : config["printers"].items()) {
        if (!printer.is_object())
            continue;
        const std::vector<std::pair<std::string, std::string>> migrations = {
            {"/printer/chamber_sensor", "/temp_sensors/chamber"},
            {"/printer/chamber_heater", "/heaters/chamber"},
        };
        if (migrate_config_keys(printer, migrations)) {
            spdlog::info("[Config] Migration v12: consolidated chamber keys for printer '{}'",
                         printer_id);
        }
        // If the now-empty /printer subkey remains, drop it.
        if (printer.contains("printer") && printer["printer"].is_object() &&
            printer["printer"].empty()) {
            printer.erase("printer");
        }
    }
}

/// Repair stale AD5X display settings from the pre-#431 preset. The original AD5X
/// preset (#235) set sleep_backlight_off=false under the assumption that backlight
/// power-off prevented wake-on-touch. #431 corrected this after hardware verification.
/// Users whose wizard ran between those commits still have the stale combination
/// (backlight never turns off at sleep).
static void migrate_v12_to_v13(json& config) {
    bool is_ad5x = false;
    if (config.value("preset", "") == "ad5x") {
        is_ad5x = true;
    } else if (config.contains("printers") && config["printers"].is_object()) {
        for (auto& [printer_id, printer] : config["printers"].items()) {
            if (printer.is_object() && printer.value("type", "") == "FlashForge Adventurer 5X") {
                is_ad5x = true;
                break;
            }
        }
    }
    if (!is_ad5x)
        return;

    if (!config.contains("display") || !config["display"].is_object())
        return;

    auto& display = config["display"];
    if (display.value("sleep_backlight_off", true) == false &&
        display.value("hardware_blank", -1) == 0) {
        display["sleep_backlight_off"] = true;
        display["hardware_blank"] = 1;
        spdlog::info("[Config] Migration v13: restored AD5X backlight-off sleep "
                     "(sleep_backlight_off=true, hardware_blank=1)");
    }
}

/// Fold legacy telemetry_config.json into settings.json. The previous
/// architecture had two sources of truth for /telemetry_enabled: TelemetryManager
/// owned telemetry_config.json; SystemSettingsManager owned settings.json's
/// /telemetry_enabled. A sync line in application.cpp clobbered the former
/// with the latter on every startup, silently disabling telemetry for users
/// whose settings.json had never had the key set. This migration preserves
/// whatever state telemetry_config.json held and then retires the file.
static void migrate_v13_to_v14(json& config, const std::string& config_path) {
    // If /telemetry_enabled is already in settings.json, no migration needed —
    // but still delete the legacy file below if it exists.
    bool has_key = config.contains("telemetry_enabled");

    if (config_path.empty()) {
        // Called from a test path without a filesystem; nothing to migrate.
        return;
    }

    fs::path legacy_path = fs::path(config_path).parent_path() / "telemetry_config.json";
    std::error_code ec;
    if (!fs::exists(legacy_path, ec)) {
        return;
    }

    try {
        std::ifstream f(legacy_path);
        json legacy;
        f >> legacy;
        if (!has_key && legacy.contains("enabled") && legacy["enabled"].is_boolean()) {
            bool legacy_enabled = legacy["enabled"].get<bool>();
            config["telemetry_enabled"] = legacy_enabled;
            spdlog::info("[Config] Migration v14: imported telemetry_enabled={} "
                         "from legacy {}",
                         legacy_enabled ? "true" : "false", legacy_path.string());
        }
    } catch (const std::exception& e) {
        spdlog::warn("[Config] Migration v14: failed to read legacy {}: {} "
                     "(leaving file in place for retry)",
                     legacy_path.string(), e.what());
        return;
    }

    fs::remove(legacy_path, ec);
    if (!ec) {
        spdlog::debug("[Config] Migration v14: removed legacy {}", legacy_path.string());
    }
}

/// v14→v15: Re-apply AD5X sleep preset for users whose printer-identify wizard
/// ran AFTER the v12→v13 migration. The wizard (pre-fix) force-wrote the stale
/// pre-#431 display config on every confirmation, undoing the v13 restore for
/// any AD5X user who ran it. The wizard block has been removed in the same
/// release that introduces this migration. Detection condition is identical to
/// v12→v13 so the fix is idempotent if the wizard is re-run on an older build.
static void migrate_v14_to_v15(json& config) {
    bool is_ad5x = false;
    if (config.value("preset", "") == "ad5x") {
        is_ad5x = true;
    } else if (config.contains("printers") && config["printers"].is_object()) {
        for (auto& [printer_id, printer] : config["printers"].items()) {
            if (printer.is_object() && printer.value("type", "") == "FlashForge Adventurer 5X") {
                is_ad5x = true;
                break;
            }
        }
    }
    if (!is_ad5x)
        return;

    if (!config.contains("display") || !config["display"].is_object())
        return;

    auto& display = config["display"];
    if (display.value("sleep_backlight_off", true) == false &&
        display.value("hardware_blank", -1) == 0) {
        display["sleep_backlight_off"] = true;
        display["hardware_blank"] = 1;
        spdlog::info("[Config] Migration v15: re-applied AD5X backlight-off sleep "
                     "after wizard-override removal");
    }
}

/// v15 → v16: Turn the screensaver off on BASIC/EMBEDDED tiers where Flying Toasters
/// causes Klipper print failures. Queues a one-time info modal via a transient flag
/// consumed by Application post-boot. Only affects type 1 (Flying Toasters); types 2/3
/// (Starfield, Pipes 3D) are left untouched because we have no evidence they cause
/// the same problem, and silently flipping an explicit user choice is worse than
/// leaving a slightly expensive screensaver running on their preferred setting.
static void migrate_v15_to_v16(json& config) {
#if defined(HELIX_SPLASH_ONLY) || defined(HELIX_WATCHDOG)
    // Splash and watchdog don't render the screensaver and don't link the
    // PlatformCapabilities object code. The version still advances; the next
    // helix-screen run will surface the migration notice if applicable.
    (void)config;
#else
    using helix::PlatformTier;
    PlatformTier tier = current_tier_for_migration();
    if (tier != PlatformTier::BASIC && tier != PlatformTier::EMBEDDED) {
        return; // STANDARD hardware keeps its setting
    }

    if (!config.contains("display") || !config["display"].is_object()) {
        return;
    }
    auto& display = config["display"];

    int current_type = display.value("screensaver_type", 0);
    if (current_type != 1) {
        return; // only migrate Flying Toasters
    }

    display["screensaver_type"] = 0;
    display["screensaver_migration_notice_pending"] = true;
    spdlog::info("[Config] Migration v16: screensaver disabled on {} tier "
                 "(was Flying Toasters); notice queued",
                 helix::platform_tier_to_string(tier));
#endif
}

/// Run all versioned migrations in sequence from current version to CURRENT_CONFIG_VERSION
static void run_versioned_migrations(json& config, const std::string& config_path = "") {
    int version = 0;
    if (config.contains("config_version")) {
        version = config["config_version"].get<int>();
    }

    if (version < 1)
        migrate_v0_to_v1(config);
    if (version < 2)
        migrate_v1_to_v2(config);
    if (version < 3)
        migrate_v2_to_v3(config);
    if (version < 4)
        migrate_v3_to_v4(config);
    if (version < 5)
        migrate_v4_to_v5(config);
    if (version < 6)
        migrate_v5_to_v6(config);
    if (version < 7)
        migrate_v6_to_v7(config);
    if (version < 8)
        migrate_v7_to_v8(config);
    if (version < 9)
        migrate_v8_to_v9(config);
    if (version < 10)
        migrate_v9_to_v10(config);
    if (version < 11)
        migrate_v10_to_v11(config);
    if (version < 12)
        migrate_v11_to_v12(config);
    if (version < 13)
        migrate_v12_to_v13(config);
    if (version < 14)
        migrate_v13_to_v14(config, config_path);
    if (version < 15)
        migrate_v14_to_v15(config);
    if (version < 16)
        migrate_v15_to_v16(config);

    config["config_version"] = CURRENT_CONFIG_VERSION;
}

/// Default root-level config - shared between init() and reset_to_defaults()
/// @param moonraker_host Host address for printer
/// @param include_user_prefs Include user preference fields (brightness, sounds, etc.)
json get_default_config(const std::string& moonraker_host, bool include_user_prefs) {
    // log_level intentionally absent - test_mode provides fallback to DEBUG
    std::string printer_id = "default";
    json printer_data = get_default_printer_config(moonraker_host);

    json config = {{"config_version", CURRENT_CONFIG_VERSION},
                   {"active_printer_id", printer_id},
                   {"log_path", "/tmp/helixscreen.log"},
                   {"dark_mode", true},
                   {"theme", {{"preset", 0}}},
                   {"display", get_default_display_config()},
                   {"gcode_viewer", {{"shading_model", "phong"}, {"tube_sides", 4}}},
                   {"input",
                    {{"scroll_throw", 25},
                     {"scroll_limit", 10},
                     {"jitter_threshold", 5},
                     {"touch_device", ""},
                     {"calibration",
                      {{"valid", false},
                       {"a", 1.0},
                       {"b", 0.0},
                       {"c", 0.0},
                       {"d", 0.0},
                       {"e", 1.0},
                       {"f", 0.0}}}}},
                   {"printers", {{"show_printer_switcher", false}, {printer_id, printer_data}}}};

    if (include_user_prefs) {
        config["brightness"] = 80;
        config["sounds_enabled"] = false;
        config["completion_alert"] = true;
        config["wizard_completed"] = false;
        config["wifi_expected"] = false;
        config["language"] = "en";
    }

    return config;
}

using helix::config_backup::find_backup;
using helix::config_backup::restore_from_backup;
using helix::config_backup::write_backup_file;
using helix::config_backup::write_rolling_backup;

/// Backup search paths in priority order (primary, fallback, legacy primary, legacy fallback).
/// Used by restore_from_backup() and find_backup() calls throughout init().
static std::vector<std::string> config_backup_search_paths() {
    return {CONFIG_BACKUP_PRIMARY, config_backup_fallback(), LEGACY_CONFIG_BACKUP_PRIMARY,
            legacy_config_backup_fallback()};
}

} // namespace

Config::Config() {}

Config* Config::get_instance() {
    if (instance == nullptr) {
        instance = new Config();
    }
    return instance;
}

void Config::init(const std::string& config_path) {
    // HELIX_CONFIG_DIR override: redirect settings into a caller-chosen
    // directory. Lets a read-only baseline install (e.g., the cosmos .ipk
    // under /usr/share/helixscreen) persist user settings into a writable
    // path — typically ~/printer_data/config/helixscreen — without first
    // requiring the in-app updater to relocate the install itself. The env
    // var supplies the DIRECTORY; we keep the caller's filename so the
    // settings.json / settings-test.json distinction is preserved.
    std::string resolved_path = config_path;
    if (const char* env_dir = std::getenv("HELIX_CONFIG_DIR");
        env_dir != nullptr && env_dir[0] != '\0') {
        std::error_code ec;
        fs::path base(env_dir);
        fs::create_directories(base, ec);
        if (fs::is_directory(base, ec)) {
            resolved_path = (base / fs::path(config_path).filename()).string();
            spdlog::info("[Config] HELIX_CONFIG_DIR override: using {}", resolved_path);
        } else {
            spdlog::warn("[Config] HELIX_CONFIG_DIR={} unusable ({}); falling back to {}", env_dir,
                         ec ? ec.message() : "not a directory", config_path);
        }
    }
    path = resolved_path;
    struct stat buffer;

    // Migration: rename helixconfig.json -> settings.json if old name exists
    fs::path old_config = fs::path(path).parent_path() / "helixconfig.json";
    if (stat(path.c_str(), &buffer) != 0 && fs::exists(old_config) && !fs::is_symlink(old_config)) {
        spdlog::info("[Config] Migrating {} -> {}", old_config.string(), path);
        std::error_code ec;
        fs::rename(old_config, path, ec);
        if (ec) {
            spdlog::warn("[Config] Migration rename failed: {} — trying copy", ec.message());
            try {
                fs::copy_file(old_config, path);
                fs::remove(old_config);
                spdlog::info("[Config] Migration complete (copy+remove)");
            } catch (const fs::filesystem_error& e) {
                spdlog::error("[Config] Migration failed: {}", e.what());
            }
        } else {
            spdlog::info("[Config] Migration complete");
        }
    } else if (stat(path.c_str(), &buffer) != 0 && fs::is_symlink(old_config)) {
        // Old config is a symlink (Pi/SonicPad: points to printer_data).
        // Don't rename symlinks — the installer handles that. Just use the
        // symlink path directly so we read/write the user's real config.
        if (stat(old_config.c_str(), &buffer) == 0) {
            path = old_config.string();
            spdlog::info("[Config] {} is a symlink — using it directly, "
                         "installer will migrate on next update",
                         old_config.string());
        } else {
            spdlog::warn("[Config] {} is a dangling symlink", old_config.string());
        }
    } else if (stat(path.c_str(), &buffer) == 0 && fs::exists(old_config)) {
        spdlog::warn("[Config] Both settings.json and helixconfig.json exist; "
                     "using settings.json (old file left in place)");
    }

    // Migrate test config unconditionally (has its own existence guard)
    fs::path old_test = fs::path(path).parent_path() / "helixconfig-test.json";
    fs::path new_test = fs::path(path).parent_path() / "settings-test.json";
    if (fs::exists(old_test) && !fs::exists(new_test)) {
        std::error_code ec;
        fs::rename(old_test, new_test, ec);
        if (!ec) {
            spdlog::info("[Config] Migrated test config: {} -> {}", old_test.string(),
                         new_test.string());
        } else {
            spdlog::warn("[Config] Test config migration failed: {}", ec.message());
        }
    }

    // Migration: Check for legacy config at old location (helixconfig.json in app root)
    // If new location doesn't exist but old location does, migrate it
    // Note: use `path` (not `config_path`) — may have been redirected to symlink above
    if (stat(path.c_str(), &buffer) != 0) {
        // Config doesn't exist - check for legacy locations
        const std::vector<std::string> legacy_paths = {
            "helixconfig.json",                  // Old location (app root)
            "/opt/helixscreen/helixconfig.json", // Legacy embedded install
        };

        for (const auto& legacy_path : legacy_paths) {
            if (stat(legacy_path.c_str(), &buffer) == 0) {
                spdlog::info("[Config] Found legacy config at {}, migrating to {}", legacy_path,
                             path);

                // Ensure config/ directory exists
                fs::path config_dir = fs::path(path).parent_path();
                if (!config_dir.empty() && !fs::exists(config_dir)) {
                    fs::create_directories(config_dir);
                }

                // Copy legacy config to new location, then remove old file
                try {
                    fs::copy_file(legacy_path, path);
                    // Remove legacy file to avoid confusion
                    fs::remove(legacy_path);
                    spdlog::info("[Config] Migration complete: {} -> {} (old file removed)",
                                 legacy_path, path);
                } catch (const fs::filesystem_error& e) {
                    spdlog::warn("[Config] Migration failed: {}", e.what());
                    // Fall through to create default config
                }
                break;
            }
        }

        // Recovery: restore config from rolling backups if missing.
        // Backups are maintained by Config::save() and survive Moonraker's
        // shutil.rmtree() wipe of the install directory.
        restore_from_backup(path, "Config", config_backup_search_paths());
    }

    // Restore helixscreen.env independently — it can be lost even if config survived
    {
        std::string env_path = (fs::path(path).parent_path() / "helixscreen.env").string();
        restore_from_backup(env_path, "helixscreen.env",
                            {ENV_BACKUP_PRIMARY, env_backup_fallback()});
    }

    bool config_modified = false;

    if (stat(path.c_str(), &buffer) == 0) {
        // Load existing config
        spdlog::info("[Config] Loading config from {}", path);
        try {
            data = json::parse(std::fstream(path));

            // Detect tarball default that replaced user config during a Moonraker
            // web update.  Moonraker type:web does rmtree() on the install dir and
            // extracts the release tarball fresh — the tarball includes a preset-based
            // settings.json with wizard_completed=false and no config_version.  If a
            // rolling backup with real user data exists, prefer it.
            if (data.value("config_version", 0) == 0) {
                std::string backup_src = find_backup(config_backup_search_paths());
                if (!backup_src.empty()) {
                    try {
                        auto backup_data = json::parse(std::fstream(backup_src));
                        if (backup_data.value("config_version", 0) > 0) {
                            spdlog::warn("[Config] Loaded config is a tarball default "
                                         "(no config_version) — restoring from backup: {}",
                                         backup_src);
                            data = std::move(backup_data);
                            config_modified = true;
                            NOTIFY_WARNING("Settings restored after update");
                        }
                    } catch (const json::exception& e) {
                        spdlog::warn("[Config] Backup parse failed during tarball detection: {}",
                                     e.what());
                    }
                }
            }
        } catch (const json::exception& e) {
            spdlog::error("[Config] Failed to parse {}: {}", path, e.what());
            CONFIG_RECORD_ERROR("file_io", "config_read_failed",
                                fmt::format("parse error: {}", e.what()));

            // Preserve the corrupt file for diagnosis
            std::string corrupt_path = path + ".corrupt";
            std::rename(path.c_str(), corrupt_path.c_str());
            spdlog::info("[Config] Corrupt config saved to {}", corrupt_path);

            // Try restoring from backup before falling back to defaults
            std::string backup_src = find_backup(config_backup_search_paths());

            bool restored = false;
            if (!backup_src.empty()) {
                try {
                    data = json::parse(std::fstream(backup_src));
                    restored = true;
                    spdlog::info("[Config] Restored from backup: {}", backup_src);
                    NOTIFY_WARNING("Settings were corrupted — restored from backup");
                } catch (const json::exception& e2) {
                    spdlog::warn("[Config] Backup also corrupt: {}", e2.what());
                }
            }

            if (!restored) {
                spdlog::warn("[Config] No valid backup — resetting to defaults");
                data = get_default_config("127.0.0.1", false);
                NOTIFY_ERROR(
                    "Settings were corrupted and could not be recovered — reset to defaults");
            }
            config_modified = true;
        }

        // Run display config migration (moves root-level display_* to /display/)
        if (migrate_display_config(data)) {
            config_modified = true;
        }

        // Migrate touch settings from /display/ to /input/
        if (migrate_config_keys(data, {{"/display/calibration", "/input/calibration"},
                                       {"/display/touch_device", "/input/touch_device"}})) {
            config_modified = true;
        }

        // Run versioned migrations (v0→v1: disable sounds for existing configs, etc.)
        // Pass path so v13→v14 can find the legacy telemetry_config.json sidecar.
        int version_before = data.value("config_version", 0);
        run_versioned_migrations(data, path);
        if (data["config_version"].get<int>() != version_before) {
            config_modified = true;
        }
    } else {
        // Create default config
        spdlog::info("[Config] Creating default config at {}", path);
        data = get_default_config("127.0.0.1", false);
        config_modified = true;
    }

    // Load active printer ID from config (must happen before df() is used)
    if (data.contains("active_printer_id") && data["active_printer_id"].is_string()) {
        active_printer_id_ = data["active_printer_id"].get<std::string>();
    }

    // Ensure printers map exists
    if (!data.contains("printers") || !data["printers"].is_object()) {
        data["printers"] = {{"default", get_default_printer_config("127.0.0.1")}};
        data["active_printer_id"] = "default";
        active_printer_id_ = "default";
        config_modified = true;
    }

    // If active_printer_id is empty or doesn't point to a valid printer object, pick first one.
    // The printers map may contain non-printer keys (e.g. show_printer_switcher as a bool),
    // so we must verify the value is an object, not just that the key exists.
    if (active_printer_id_.empty() || !data["printers"].contains(active_printer_id_) ||
        !data["printers"][active_printer_id_].is_object()) {
        active_printer_id_.clear();
        for (auto& [key, val] : data["printers"].items()) {
            if (val.is_object()) {
                active_printer_id_ = key;
                break;
            }
        }
        if (!active_printer_id_.empty()) {
            data["active_printer_id"] = active_printer_id_;
            config_modified = true;
            spdlog::info("[Config] Auto-selected active printer: {}", active_printer_id_);
        }
    }

    // Ensure active printer has required fields with defaults
    if (!active_printer_id_.empty()) {
        auto printer_ptr = json::json_pointer("/printers/" + active_printer_id_);
        auto& printer = data[printer_ptr];
        if (printer.is_null()) {
            data[printer_ptr] = get_default_printer_config("127.0.0.1");
            config_modified = true;
        } else {
            // Ensure heaters exists with defaults
            auto& heaters = data[json::json_pointer(df() + "heaters")];
            if (heaters.is_null()) {
                data[json::json_pointer(df() + "heaters")] = {{"bed", "heater_bed"},
                                                              {"hotend", "extruder"}};
                config_modified = true;
            }

            // Ensure temp_sensors exists with defaults
            auto& temp_sensors = data[json::json_pointer(df() + "temp_sensors")];
            if (temp_sensors.is_null()) {
                data[json::json_pointer(df() + "temp_sensors")] = {{"bed", "heater_bed"},
                                                                   {"hotend", "extruder"}};
                config_modified = true;
            }

            // Ensure fans exists with defaults
            auto& fans = data[json::json_pointer(df() + "fans")];
            if (fans.is_null()) {
                data[json::json_pointer(df() + "fans")] = {{"part", "fan"},
                                                           {"hotend", "heater_fan hotend_fan"}};
                config_modified = true;
            }

            // Ensure leds exists with defaults
            auto& leds = data[json::json_pointer(df() + "leds")];
            if (leds.is_null()) {
                data[json::json_pointer(df() + "leds")] = {{"strip", "neopixel chamber_light"}};
                config_modified = true;
            }

            // Ensure leds/selected array exists (for multi-LED support)
            auto& leds_selected = data[json::json_pointer(df() + "leds/selected")];
            if (leds_selected.is_null()) {
                // Check if there's a legacy strip value to migrate
                auto& strip = data[json::json_pointer(df() + "leds/strip")];
                if (!strip.is_null() && strip.is_string()) {
                    std::string led = strip.get<std::string>();
                    if (!led.empty()) {
                        data[json::json_pointer(df() + "leds/selected")] = json::array({led});
                    } else {
                        data[json::json_pointer(df() + "leds/selected")] = json::array();
                    }
                } else {
                    data[json::json_pointer(df() + "leds/selected")] = json::array();
                }
                config_modified = true;
            }

            // Ensure extra_sensors exists (empty object for user additions)
            auto& extra_sensors = data[json::json_pointer(df() + "extra_sensors")];
            if (extra_sensors.is_null()) {
                data[json::json_pointer(df() + "extra_sensors")] = json::object();
                config_modified = true;
            }

            // Ensure hardware section exists
            auto& hardware = data[json::json_pointer(df() + "hardware")];
            if (hardware.is_null()) {
                data[json::json_pointer(df() + "hardware")] = {{"optional", json::array()},
                                                               {"expected", json::array()},
                                                               {"last_snapshot", json::object()}};
                config_modified = true;
            }

            // Ensure default_macros exists
            auto& default_macros = data[json::json_pointer(df() + "default_macros")];
            if (default_macros.is_null()) {
                data[json::json_pointer(df() + "default_macros")] = get_default_macros();
                config_modified = true;
            }
        }
    }

    // log_level intentionally NOT migrated - absence allows test_mode fallback

    // Ensure display section exists with defaults
    if (!data.contains("display")) {
        data["display"] = get_default_display_config();
        config_modified = true;
    } else {
        // Ensure all display subsections exist with defaults
        auto display_defaults = get_default_display_config();
        auto& display = data["display"];

        for (auto& [key, value] : display_defaults.items()) {
            if (!display.contains(key)) {
                display[key] = value;
                config_modified = true;
            }
        }
    }

    // Ensure input section exists with defaults (scroll settings + touch calibration)
    if (!data.contains("input")) {
        data["input"] = {{"scroll_throw", 25},
                         {"scroll_limit", 10},
                         {"jitter_threshold", 5},
                         {"touch_device", ""},
                         {"calibration",
                          {{"valid", false},
                           {"a", 1.0},
                           {"b", 0.0},
                           {"c", 0.0},
                           {"d", 0.0},
                           {"e", 1.0},
                           {"f", 0.0}}}};
        config_modified = true;
    } else {
        // Ensure all input subsections exist with defaults
        auto& input = data["input"];

        // Ensure scroll settings exist
        if (!input.contains("scroll_throw")) {
            input["scroll_throw"] = 25;
            config_modified = true;
        }
        if (!input.contains("scroll_limit")) {
            input["scroll_limit"] = 10;
            config_modified = true;
        }
        if (!input.contains("touch_device")) {
            input["touch_device"] = "";
            config_modified = true;
        }
        if (!input.contains("jitter_threshold")) {
            input["jitter_threshold"] = 5;
            config_modified = true;
        }

        // Ensure calibration subsection exists with all required fields
        if (!input.contains("calibration")) {
            input["calibration"] = {{"valid", false}, {"a", 1.0}, {"b", 0.0}, {"c", 0.0},
                                    {"d", 0.0},       {"e", 1.0}, {"f", 0.0}};
            config_modified = true;
        } else {
            // Ensure all calibration fields exist
            auto& cal = input["calibration"];
            const json cal_defaults = {{"valid", false}, {"a", 1.0}, {"b", 0.0}, {"c", 0.0},
                                       {"d", 0.0},       {"e", 1.0}, {"f", 0.0}};
            for (auto& [key, value] : cal_defaults.items()) {
                if (!cal.contains(key)) {
                    cal[key] = value;
                    config_modified = true;
                }
            }
        }
    }

    // Probe for read-only filesystem before attempting any writes.
    // Try creating a small test file in the config directory — if it fails
    // with EROFS or EACCES, enable read-only mode and skip all future saves.
    {
        fs::path config_dir = fs::path(path).parent_path();
        std::string probe_path = (config_dir / ".helix-write-probe").string();
        std::ofstream probe(probe_path);
        if (!probe.is_open()) {
            int err = errno;
            if (err == EROFS || err == EACCES) {
                read_only_mode_ = true;
                spdlog::warn("[Config] Read-only filesystem detected ({}): "
                             "config changes will not be persisted",
                             strerror(err));
            }
        } else {
            probe.close();
            std::remove(probe_path.c_str());
        }
    }

    // Save updated config with any new defaults or migrations.
    // Use the atomic temp+rename path — a truncating write directly to the
    // live config would corrupt it on power loss mid-write at every boot.
    // Deliberately NOT Config::save(): that also writes the rolling backup,
    // which must stay skipped while the wizard is required (see below).
    if (config_modified && !read_only_mode_) {
        try {
            if (save_to_disk()) {
                spdlog::debug("[Config] Saved updated config to {}", path);
            } else {
                spdlog::warn("[Config] Failed to persist migrated config to {}", path);
            }
        } catch (const std::exception& e) {
            spdlog::warn("[Config] Failed to persist migrated config: {}", e.what());
        }
    }

    // Maintain a rolling backup on startup — ensures backup freshness even if
    // the user never explicitly saves settings.  Skip when the loaded config is
    // a tarball default (wizard not yet completed, no real user data) to avoid
    // poisoning the backup with preset defaults that would break future recovery.
    if (!is_wizard_required()) {
        write_rolling_backup(path, CONFIG_BACKUP_PRIMARY, config_backup_fallback());
    }

    // Back up helixscreen.env outside install dir (env only changes at startup via launcher)
    {
        std::string env_path = (fs::path(path).parent_path() / "helixscreen.env").string();
        write_rolling_backup(env_path, ENV_BACKUP_PRIMARY, env_backup_fallback());
    }

    spdlog::debug("[Config] initialized: moonraker={}:{}",
                  get<std::string>(df() + "moonraker_host", "127.0.0.1"),
                  get<int>(df() + "moonraker_port", 7125));
}

std::string Config::df() {
    if (active_printer_id_.empty()) {
        spdlog::warn("[Config] df() called with no active printer, using 'default'");
        return "/printers/default/";
    }
    return "/printers/" + active_printer_id_ + "/";
}

// ============================================================================
// Multi-printer support
// ============================================================================

std::string Config::get_active_printer_id() const {
    return active_printer_id_;
}

bool Config::set_active_printer(const std::string& printer_id) {
    if (!data.contains("printers") || !data["printers"].contains(printer_id) ||
        !data["printers"][printer_id].is_object()) {
        spdlog::error("[Config] Cannot switch to unknown printer '{}'", printer_id);
        return false;
    }
    active_printer_id_ = printer_id;
    data["active_printer_id"] = printer_id;
    spdlog::info("[Config] Switched active printer to '{}'", printer_id);
    return true;
}

std::vector<std::string> Config::get_printer_ids() const {
    std::vector<std::string> ids;
    if (data.contains("printers") && data["printers"].is_object()) {
        for (auto& [key, val] : data["printers"].items()) {
            if (!val.is_object())
                continue;
            ids.push_back(key);
        }
    }
    return ids;
}

void Config::add_printer(const std::string& printer_id, const json& printer_data) {
    if (!data.contains("printers")) {
        data["printers"] = json::object();
    }
    data["printers"][printer_id] = printer_data;
    spdlog::info("[Config] Added printer '{}'", printer_id);
}

void Config::remove_printer(const std::string& printer_id) {
    if (!data.contains("printers") || !data["printers"].contains(printer_id)) {
        spdlog::warn("[Config] Cannot remove non-existent printer '{}'", printer_id);
        return;
    }

    // Prevent removing the last printer
    if (data["printers"].size() <= 1) {
        spdlog::error("[Config] Cannot remove last printer '{}' — at least one printer must exist",
                      printer_id);
        return;
    }

    data["printers"].erase(printer_id);
    spdlog::info("[Config] Removed printer '{}'", printer_id);

    // If we just removed the active printer, switch to the first remaining one
    if (active_printer_id_ == printer_id) {
        auto remaining_id = data["printers"].begin().key();
        active_printer_id_ = remaining_id;
        data["active_printer_id"] = remaining_id;
        spdlog::info("[Config] Auto-switched to printer '{}' after removing '{}'", remaining_id,
                     printer_id);
    }
}

std::string Config::slugify(const std::string& name) {
    std::string result;
    result.reserve(name.size());

    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            // Replace non-alphanumeric with hyphen
            if (!result.empty() && result.back() != '-') {
                result += '-';
            }
        }
    }

    // Strip trailing hyphens
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }

    // Strip leading hyphens
    size_t start = 0;
    while (start < result.size() && result[start] == '-') {
        ++start;
    }
    if (start > 0) {
        result = result.substr(start);
    }

    return result.empty() ? "default" : result;
}

std::string Config::get_path() {
    return path;
}

json& Config::get_json(const std::string& json_path) {
    return data[json::json_pointer(json_path)];
}

/// Map common errno values to user-friendly descriptions
static std::string errno_reason(int err) {
    switch (err) {
    case ENOSPC:
        return "disk full";
    case EROFS:
        return "read-only filesystem";
    case EACCES:
        return "permission denied";
    default:
        return strerror(err);
    }
}

bool Config::save_to_disk() {
        // Resolve symlinks so atomic rename targets the real file, not the symlink
        std::string target_path = path;
        {
            std::error_code ec;
            if (fs::is_symlink(path, ec)) {
                auto real = fs::canonical(path, ec);
                if (!ec) {
                    spdlog::debug("[Config] Resolved symlink {} -> {}", path, real.string());
                    target_path = real.string();
                }
            }
        }

        // Atomic save: write to temp file, then rename to avoid partial writes on crash/power loss
        std::string tmp_path = target_path + ".tmp";
        {
            std::ofstream o(tmp_path);
            if (!o.is_open()) {
                std::string reason = errno_reason(errno);
                NOTIFY_ERROR("Could not save settings: {}", reason);
                LOG_ERROR_INTERNAL("Failed to open temp file for writing: {} ({})", tmp_path,
                                   reason);
                CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                                    fmt::format("open failed: {}", reason));
                return false;
            }

            o << std::setw(2) << data << std::endl;
            o.flush();

            if (!o.good()) {
                std::string reason = errno_reason(errno);
                NOTIFY_ERROR("Failed to save settings: {}", reason);
                LOG_ERROR_INTERNAL("Failed to write config to {}: {}", tmp_path, reason);
                CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                                    fmt::format("write error: {}", reason));
                std::remove(tmp_path.c_str());
                return false;
            }
        }

        if (std::rename(tmp_path.c_str(), target_path.c_str()) != 0) {
            NOTIFY_ERROR("Failed to save configuration file");
            LOG_ERROR_INTERNAL("Failed to rename temp file '{}' to '{}': {}", tmp_path, target_path,
                               strerror(errno));
            CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                                fmt::format("rename failed: {}", strerror(errno)));
            std::remove(tmp_path.c_str());
            return false;
        }

        spdlog::trace("[Config] saved successfully to {}", path);
        return true;
}

bool Config::save() {
    if (path.empty()) {
        spdlog::trace("[Config] Skipping save (no config path set)");
        return true;
    }

    if (read_only_mode_) {
        spdlog::warn("[Config] Skipping save — filesystem is read-only");
        return false;
    }

    spdlog::trace("[Config] Saving config to {}", path);

    try {
        if (!save_to_disk()) {
            return false;
        }

        // Maintain rolling backup outside install dir (survives Moonraker wipes)
        write_rolling_backup(path, CONFIG_BACKUP_PRIMARY, config_backup_fallback());

        return true;

    } catch (const std::exception& e) {
        NOTIFY_ERROR("Failed to save configuration: {}", e.what());
        LOG_ERROR_INTERNAL("Exception while saving config to {}: {}", path, e.what());
        CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                            fmt::format("exception: {}", e.what()));
        return false;
    }
}

bool Config::is_read_only() const {
    return read_only_mode_;
}

bool Config::has_preset() const {
    if (data.contains("preset") && data["preset"].is_string()) {
        return !data["preset"].get<std::string>().empty();
    }
    return false;
}

std::string Config::get_preset() const {
    if (data.contains("preset") && data["preset"].is_string()) {
        return data["preset"].get<std::string>();
    }
    return "";
}

void Config::set_preset(const std::string& preset_name) {
    if (preset_name.empty()) {
        return;
    }
    data["preset"] = preset_name;
    spdlog::info("[Config] Preset set to '{}'", preset_name);
}

bool Config::apply_preset_file(const std::string& preset_name) {
    // Guard: only apply if wizard hasn't been completed for this printer
    if (get<bool>(df() + "wizard_completed", false)) {
        spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
        return false;
    }

    // Resolve via find_readable so the writable user dir wins, then fall back
    // to the shipped read-only seed bundle at $HELIX_DATA_DIR/assets/config/presets/.
    // The seed location is where install tarballs land presets — looking only in
    // the writable config dir would miss every preset on a fresh install.
    std::string preset_relpath = std::string("presets/") + preset_name + ".json";
    std::string preset_path = helix::find_readable(preset_relpath);
    if (!fs::exists(preset_path)) {
        spdlog::warn("[Config] Preset file not found: {} (looked in writable + seed bundle)",
                     preset_path);
        return false;
    }

    // Load and parse preset JSON
    json preset_json;
    try {
        preset_json = json::parse(std::fstream(preset_path));
    } catch (const json::exception& e) {
        spdlog::error("[Config] Failed to parse preset '{}': {}", preset_path, e.what());
        return false;
    }

    // Deep-merge the entire "printer" section into the active printer subtree.
    // Guarded by wizard_completed above, so this only runs on fresh-install / pre-wizard
    // state — safe to overwrite any scaffolded defaults (e.g. empty moonraker_host) with
    // preset values. Covers all preset keys (hardware, moonraker_host/port, input, etc.)
    // without a hardcoded allowlist.
    if (preset_json.contains("printer") && preset_json["printer"].is_object() &&
        !active_printer_id_.empty()) {
        auto& printer_node = data["printers"][active_printer_id_];
        if (!printer_node.is_object()) {
            printer_node = json::object();
        }
        printer_node.merge_patch(preset_json["printer"]);
    }

    // Deep-merge device-level display settings (preserves keys not in preset)
    if (preset_json.contains("display") && preset_json["display"].is_object()) {
        if (!data.contains("display") || !data["display"].is_object()) {
            data["display"] = json::object();
        }
        data["display"].merge_patch(preset_json["display"]);
    }

#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
    // Populate per-printer `type` from the database entry whose `preset` field matches.
    // Without this, the home panel's image widget has no printer_type to look up and
    // falls back to the generic CoreXY image. Only the main app applies presets at
    // runtime — splash/watchdog just read existing config — so PrinterDetector (which
    // pulls in the full printer database) is excluded from those binaries.
    std::string type_key = df() + helix::wizard::PRINTER_TYPE;
    if (get<std::string>(type_key, "").empty()) {
        std::string type_name = PrinterDetector::get_name_for_preset(preset_name);
        if (!type_name.empty()) {
            set<std::string>(type_key, type_name);
            spdlog::info("[Config] Preset '{}' resolved to printer type '{}'", preset_name,
                         type_name);
        } else {
            spdlog::warn("[Config] No database entry matches preset '{}' — printer type unset",
                         preset_name);
        }
    }
#endif

    spdlog::info("[Config] Applied preset '{}' to active printer", preset_name);
    save();
    return true;
}

bool Config::is_wizard_required() {
    // Check per-printer wizard_completed first (v3 config)
    if (!active_printer_id_.empty()) {
        json::json_pointer printer_ptr(df() + "wizard_completed");
        if (data.contains(printer_ptr)) {
            auto& wc = data[printer_ptr];
            if (wc.is_boolean()) {
                bool is_completed = wc.get<bool>();
                spdlog::trace("[Config] Per-printer wizard_completed = {}", is_completed);
                return !is_completed;
            }
        }
    }

    // Fall back to root-level wizard_completed (backward compat)
    json::json_pointer ptr("/wizard_completed");
    if (data.contains(ptr)) {
        auto& wizard_completed = data[ptr];
        if (wizard_completed.is_boolean()) {
            bool is_completed = wizard_completed.get<bool>();
            spdlog::trace("[Config] Root wizard_completed flag = {}", is_completed);
            return !is_completed;
        }
        spdlog::warn("[Config] wizard_completed has invalid type, treating as unset");
    }

    // No flag set - wizard has never been run
    spdlog::debug("[Config] No wizard_completed flag found, wizard required");
    return true;
}

bool Config::is_wifi_expected() {
    return get<bool>("/wifi_expected", false);
}

void Config::set_wifi_expected(bool expected) {
    set("/wifi_expected", expected);
}

std::string Config::get_language() {
    return get<std::string>("/language", "en");
}

void Config::set_language(const std::string& lang) {
    set("/language", lang);
}

bool Config::is_beta_features_enabled() {
#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
    // In test mode, default to true unless explicitly set to false
    auto* rt = get_runtime_config();
    if (rt && rt->is_test_mode()) {
        return get<bool>("/beta_features", true);
    }
#endif

    return get<bool>("/beta_features", false);
}

void Config::reset_to_defaults() {
    spdlog::info("[Config] Resetting configuration to factory defaults");

    // Reset to default configuration with empty moonraker_host (requires reconfiguration)
    // and include user preferences (brightness, sounds, etc.) with wizard_completed=false
    data = get_default_config("", true);

    spdlog::info("[Config] Configuration reset to defaults. Wizard will run on next startup.");
}

MacroConfig Config::get_macro(const std::string& key, const MacroConfig& default_val) {
    try {
        std::string path = df() + "default_macros/" + key;
        json::json_pointer ptr(path);

        if (!data.contains(ptr)) {
            spdlog::trace("[Config] Macro '{}' not found, using default", key);
            return default_val;
        }

        const auto& val = data[ptr];

        // Handle string format (backward compatibility): use as both label and gcode
        if (val.is_string()) {
            std::string macro = val.get<std::string>();
            spdlog::trace("[Config] Macro '{}' is string format: '{}'", key, macro);
            return {macro, macro};
        }

        // Handle object format: {label, gcode}
        if (val.is_object()) {
            MacroConfig result;
            result.label = val.value("label", default_val.label);
            result.gcode = val.value("gcode", default_val.gcode);
            spdlog::trace("[Config] Macro '{}': label='{}', gcode='{}'", key, result.label,
                          result.gcode);
            return result;
        }

        spdlog::warn("[Config] Macro '{}' has unexpected type, using default", key);
        return default_val;

    } catch (const std::exception& e) {
        spdlog::warn("[Config] Error reading macro '{}': {}", key, e.what());
        return default_val;
    }
}
