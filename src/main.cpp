#include "PCH.h"

#include "Framework/Framework.h"

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <utility>

namespace {

/**
 * @brief Sets up the global log file for the plugin using spdlog
 */
void setupLog()
{
    // Resolve the SKSE log directory (Documents/My Games/.../SKSE)
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) {
        SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    }

    // Create a truncating file sink named after the plugin and make it the default logger
    auto logFilePath = *logsFolder / (std::string(PLUGIN_NAME) + ".log");
    auto fileLogger = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto logger = std::make_shared<spdlog::logger>("log", std::move(fileLogger));

    // Log everything and flush per message so crashes don't lose the tail of the log
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}

} // namespace

//
// CommonLibSSE-NG / SKSE Exports
//

SKSEPluginInfo(.Version = REL::Version {0,
                                        1,
                                        0,
                                        0},
               .Name = "OpenFixes",
               .Author = "hakasapl",
               .StructCompatibility = SKSE::StructCompatibility::Independent,
               .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

    SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    setupLog();

    const auto version = REL::Module::get().version();
    spdlog::info("{} {} loading (runtime {})", PLUGIN_NAME, PLUGIN_VERSION, version.string("."));

    OpenFixes::Settings::load();
    if (OpenFixes::Settings::getBool("General", "DebugLogging", false)) {
        spdlog::set_level(spdlog::level::debug);
    }

    OpenFixes::Framework::load();

    spdlog::info("{} loaded", PLUGIN_NAME);
    return true;
}
