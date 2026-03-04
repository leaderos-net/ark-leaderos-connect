// ============================================================
//  LeaderOS Connect - ARK: Survival Evolved (AseApi)
//
//  Connects the ARK server to the LeaderOS web panel.
//  Periodically polls the LeaderOS API for pending commands
//  and executes them on the server. Supports offline queuing:
//  if a target player is not online, commands are stored in a
//  local JSON file and flushed when they next connect.
//
//  Console commands (server-side only):
//    leaderos.poll    — trigger an immediate queue poll
//    leaderos.reload  — reload config.json without restart
//    leaderos.debug   — toggle verbose debug logging
//    leaderos.status  — print current plugin state to log
// ============================================================

// ---------------------------------------------------------------------------
// UNICODE must be defined before any Windows or UE headers so that
// TCHAR resolves to wchar_t throughout the entire translation unit.
// Both httplib and the Unreal Engine runtime require this to be consistent.
// ---------------------------------------------------------------------------
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// ---------------------------------------------------------------------------
// Prevent Windows.h from auto-including the legacy WinSock v1 header.
// httplib.h needs to include WinSock2.h itself; if winsock.h (v1) is already
// loaded when WinSock2.h is included, the compiler will emit redefinition
// errors for dozens of socket types. Defining _WINSOCKAPI_ blocks that.
// ---------------------------------------------------------------------------
#define _WINSOCKAPI_

// Suppress MSVC deprecation warnings that originate inside STL / OpenSSL headers
#define _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
#define _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS

// Tell cpp-httplib to compile with OpenSSL (required for HTTPS)
#define CPPHTTPLIB_OPENSSL_SUPPORT

// ---------------------------------------------------------------------------
// Include order matters:
//   1. httplib  — loads WinSock2.h before anything else
//   2. Standard library headers
//   3. json.hpp — header-only JSON library (nlohmann)
//   4. AseApi   — Unreal Engine / ARK headers (must come after WinSock2)
// ---------------------------------------------------------------------------
#include "httplib.h"

#include <fstream>
#include <thread>
#include <mutex>
#include <json.hpp>

#include <API/ARK/Ark.h>
#include <Logger/Logger.h>
#include <Timer.h>
#include <Tools.h>

#pragma comment(lib, "ArkApi.lib")

// ============================================================
//  Configuration
// ============================================================

// All values are loaded from config.json at startup and can be
// reloaded live with the "leaderos.reload" console command.
struct Config
{
    std::string WebsiteURL;    // Base URL of the LeaderOS panel (must start with https://)
    std::string APIKey;        // API key for authenticating requests to LeaderOS
    std::string ConnectToken;  // Unique server token used to identify this server
    bool        DebugMode = false; // When true, verbose HTTP and poll logs are printed
    bool        CheckOnline = true;  // When true, commands for offline players are queued
    int         FreqMinutes = 2;     // How often (in minutes) the queue is polled
};

static Config g_Config;

// Global running flag — set to false on Plugin_Unload to stop the poll loop
static bool g_Running = false;

// ============================================================
//  Logging helpers
// ============================================================

// Convenience macros that prefix every log entry with [LeaderOS]
// LOG_DEBUG only prints when DebugMode is enabled in config
#define LOG_INFO(msg)  Log::GetLog()->info("[LeaderOS] {}", (msg))
#define LOG_DEBUG(msg) if (g_Config.DebugMode) Log::GetLog()->info("[LeaderOS][DEBUG] {}", (msg))
#define LOG_ERROR(msg) Log::GetLog()->error("[LeaderOS][ERROR] {}", (msg))

// ============================================================
//  Config loader
// ============================================================

static bool LoadConfig()
{
    // Build the absolute path to config.json relative to the server root
    const std::string path =
        ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/LeaderOSConnect/config.json";

    std::ifstream f(path);
    if (!f.is_open()) { LOG_ERROR("config.json not found: " + path); return false; }

    nlohmann::json j;
    try { f >> j; }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to parse config.json: " + std::string(e.what()));
        return false;
    }

    // Read each field, falling back to safe defaults if a key is missing
    g_Config.WebsiteURL = j.value("WebsiteURL", "");
    g_Config.APIKey = j.value("APIKey", "");
    g_Config.ConnectToken = j.value("ConnectToken", "");
    g_Config.DebugMode = j.value("DebugMode", false);
    g_Config.CheckOnline = j.value("CheckOnline", true);
    g_Config.FreqMinutes = j.value("FreqMinutes", 2);

    // Strip any trailing slashes from the URL to keep path building clean
    while (!g_Config.WebsiteURL.empty() && g_Config.WebsiteURL.back() == '/')
        g_Config.WebsiteURL.pop_back();

    // Validate required fields before allowing the plugin to start
    std::vector<std::string> errors;
    if (g_Config.WebsiteURL.empty())
        errors.push_back("WebsiteURL is empty.");
    else if (g_Config.WebsiteURL.substr(0, 8) != "https://")
        errors.push_back("WebsiteURL must start with 'https://'.");
    if (g_Config.APIKey.empty() || g_Config.APIKey == "YOUR_API_KEY_HERE")
        errors.push_back("APIKey is not set.");
    if (g_Config.ConnectToken.empty() || g_Config.ConnectToken == "YOUR_SERVER_TOKEN_HERE")
        errors.push_back("ConnectToken is not set.");

    if (!errors.empty())
    {
        LOG_ERROR("Plugin could not start due to configuration errors:");
        for (const auto& e : errors) LOG_ERROR("  - " + e);
        return false;
    }
    return true;
}

// ============================================================
//  HTTP helpers
// ============================================================

// Simple result struct returned by all HTTP calls
struct HttpResult { bool ok = false; int code = 0; std::string body; };

// Performs an HTTPS GET to /api/<endpoint> with the API key header.
// Returns ok=false on network error or non-2xx status.
static HttpResult DoGet(const std::string& endpoint)
{
    HttpResult r;
    const std::string path = "/api/" + endpoint;
    LOG_DEBUG("GET " + g_Config.WebsiteURL + path);
    try
    {
        // Extract host and optional port from the configured URL
        std::string host = g_Config.WebsiteURL.substr(g_Config.WebsiteURL.find("://") + 3);
        int port = 443;
        auto colon = host.rfind(':');
        if (colon != std::string::npos)
        {
            port = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        }

        httplib::SSLClient cli(host, port);
        cli.enable_server_certificate_verification(false); // Self-signed certs are accepted
        cli.set_connection_timeout(10);
        cli.set_read_timeout(15);

        auto res = cli.Get(path.c_str(), httplib::Headers{ {"X-Api-Key", g_Config.APIKey} });
        if (res) { r.ok = (res->status >= 200 && res->status < 300); r.code = res->status; r.body = res->body; }
        else LOG_ERROR("GET " + endpoint + " -> network error");
    }
    catch (const std::exception& e) { LOG_ERROR("GET exception: " + std::string(e.what())); }
    return r;
}

// Performs an HTTPS POST to /api/<endpoint> with form-encoded params and the API key header.
static HttpResult DoPost(const std::string& endpoint, const httplib::Params& params)
{
    HttpResult r;
    const std::string path = "/api/" + endpoint;
    LOG_DEBUG("POST " + g_Config.WebsiteURL + path);
    try
    {
        std::string host = g_Config.WebsiteURL.substr(g_Config.WebsiteURL.find("://") + 3);
        int port = 443;
        auto colon = host.rfind(':');
        if (colon != std::string::npos)
        {
            port = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        }

        httplib::SSLClient cli(host, port);
        cli.enable_server_certificate_verification(false);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(15);

        auto res = cli.Post(path.c_str(), httplib::Headers{ {"X-Api-Key", g_Config.APIKey} }, params);
        if (res) { r.ok = (res->status >= 200 && res->status < 300); r.code = res->status; r.body = res->body; }
        else LOG_ERROR("POST " + endpoint + " -> network error");
    }
    catch (const std::exception& e) { LOG_ERROR("POST exception: " + std::string(e.what())); }
    return r;
}

// ============================================================
//  Offline / pending command queue
// ============================================================
//
//  When CheckOnline is true and a target player is not connected,
//  commands are written to a local JSON file on disk. The file is
//  read and flushed the moment that player joins the server.
//
//  File format:
//  [
//    { "steamid": "76561198000000000", "cmds": ["cmd1", "cmd2"] },
//    ...
//  ]

static std::mutex g_PendingMutex;

// Returns the absolute path to the pending commands file
static std::string PendingPath()
{
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/LeaderOSConnect/leaderos_pending.json";
}

// Loads and returns the pending file contents. Returns an empty array on failure.
static nlohmann::json PendingLoad()
{
    std::lock_guard<std::mutex> lk(g_PendingMutex);
    std::ifstream f(PendingPath());
    if (!f.is_open()) return nlohmann::json::array();
    nlohmann::json j;
    try { f >> j; }
    catch (...) {}
    return j.is_array() ? j : nlohmann::json::array();
}

// Writes the given JSON array back to disk
static void PendingSave(const nlohmann::json& data)
{
    std::lock_guard<std::mutex> lk(g_PendingMutex);
    std::ofstream f(PendingPath());
    f << data.dump(2);
}

// Appends a single command to the pending list for the given Steam ID.
// If an entry for that player already exists, the command is appended to it.
static void PendingAdd(const std::string& steam_id, const std::string& cmd)
{
    auto data = PendingLoad();
    for (auto& entry : data)
    {
        if (entry.value("steamid", "") == steam_id)
        {
            entry["cmds"].push_back(cmd);
            PendingSave(data);
            return;
        }
    }
    // No existing entry — create a new one
    data.push_back({ {"steamid", steam_id}, {"cmds", nlohmann::json::array({cmd})} });
    PendingSave(data);
}

// Removes and returns all pending commands for the given Steam ID.
// Returns an empty array if no pending commands exist for that player.
static nlohmann::json PendingFlush(const std::string& steam_id)
{
    auto data = PendingLoad();
    for (auto it = data.begin(); it != data.end(); ++it)
    {
        if ((*it).value("steamid", "") == steam_id)
        {
            auto cmds = (*it)["cmds"];
            data.erase(it);
            PendingSave(data);
            return cmds;
        }
    }
    return nlohmann::json::array();
}

// ============================================================
//  Player helpers
// ============================================================

// Looks up a connected player by their 64-bit Steam ID string.
// Returns nullptr if the player is not currently online.
static AShooterPlayerController* FindPlayer(const std::string& steam_id)
{
    try
    {
        uint64 sid = std::stoull(steam_id);
        return ArkApi::GetApiUtils().FindPlayerFromSteamId(sid);
    }
    catch (...) { return nullptr; }
}

// ============================================================
//  Command execution
// ============================================================

// Replaces all occurrences of 'from' with 'to' inside string 's' in-place
static void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Executes a single console command on the server.
//
// Strategy:
//   1. If the target player is online, use their controller as the executor
//      so that player-specific tokens ({steamid}, {name}) are substituted.
//   2. If the target player is offline, fall back to the first available
//      player controller on the server. This allows global commands such as
//      "ServerChat ..." to still reach all connected players.
//   3. Before calling ConsoleCommand, temporarily elevate the executor to
//      admin+cheat status so that privileged commands are accepted.
//      The original flags are restored immediately after the call.
//   4. If there are no players at all (empty server), the command is skipped
//      and logged — it will be retried on the next poll cycle.
static void RunCommand(std::string cmd, const std::string& steam_id)
{
    // Try to find the target player first so we can substitute their tokens
    AShooterPlayerController* ply = FindPlayer(steam_id);
    if (ply != nullptr)
    {
        // Substitute {steamid} and {name} placeholders in the command string
        ReplaceAll(cmd, "{steamid}", steam_id);
        FString name_fstr = ArkApi::IApiUtils::GetCharacterName(ply);
        std::string name_str = ArkApi::Tools::Utf8Encode(
            std::wstring(reinterpret_cast<const wchar_t*>(*name_fstr)));
        ReplaceAll(cmd, "{name}", name_str);
    }

    LOG_INFO("Executing: " + cmd);

    // Convert the command string to FString for the UE console API
    std::wstring wcmd = ArkApi::Tools::Utf8Decode(cmd);
    FString fcmd(wcmd.c_str());

    // Determine which player controller will run the command.
    // Prefer the target player; fall back to any connected player.
    APlayerController* executor = static_cast<APlayerController*>(ply);
    if (executor == nullptr)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (world != nullptr)
            executor = world->GetFirstPlayerController();
    }

    if (executor != nullptr)
    {
        AShooterPlayerController* shooterPC = static_cast<AShooterPlayerController*>(executor);

        // Save original privilege flags so we can restore them after the call
        const bool was_cheat = shooterPC->bCheatPlayer()();
        const bool was_admin = shooterPC->bIsAdmin()();

        // Elevate privileges:
        //   bIsAdmin     — grants access to admin-only commands
        //   bCheatPlayer — grants access to cheat-prefixed commands (e.g. "cheat GiveItem ...")
        shooterPC->bCheatPlayer() = true;
        shooterPC->bIsAdmin() = true;

        FString result;
        executor->ConsoleCommand(&result, &fcmd, false);

        // Restore original flags — do not leave the player permanently elevated
        shooterPC->bCheatPlayer() = was_cheat;
        shooterPC->bIsAdmin() = was_admin;
    }
    else
    {
        // Server is completely empty — no controller is available to run the command.
        // IMPORTANT: This command has already been marked as executed on the LeaderOS
        // side during the validate call. It will NOT be retried and is permanently lost.
        // To prevent this situation, set CheckOnline=true in config.json so that
        // commands for offline players are queued to disk and replayed on next login.
        LOG_ERROR("No players online — command lost (already validated on server): " + cmd);
    }
}

// ============================================================
//  Queue polling
// ============================================================

// Decides whether to run commands now or queue them for later.
// If CheckOnline is enabled and the target player is offline,
// each command is written to the pending file instead of being executed.
static void ExecuteCommands(const nlohmann::json& cmds, const std::string& steam_id)
{
    if (g_Config.CheckOnline && FindPlayer(steam_id) == nullptr)
    {
        LOG_INFO("Player " + steam_id + " is offline. Queuing " + std::to_string(cmds.size()) + " command(s).");
        for (const auto& c : cmds)
            PendingAdd(steam_id, c.get<std::string>());
        return;
    }

    // Schedule each command with a 1-second gap between them to avoid flooding
    int delay = 0;
    for (const auto& c : cmds)
    {
        std::string cmd = c.get<std::string>();
        API::Timer::Get().DelayExecute([cmd, steam_id]() { RunCommand(cmd, steam_id); }, delay);
        delay += 1;
    }
}

// Sends the raw command IDs from the queue to the validate endpoint.
// The API responds with the actual command strings and the target Steam ID.
// Runs in a detached background thread to avoid blocking the game thread.
static void ValidateAndExecute(std::vector<std::string> ids)
{
    std::thread([ids = std::move(ids)]()
        {
            try
            {
                // Build form params: token + indexed array of command IDs
                httplib::Params params;
                params.emplace("token", g_Config.ConnectToken);
                for (size_t i = 0; i < ids.size(); i++)
                    params.emplace("commands[" + std::to_string(i) + "]", ids[i]);

                auto r = DoPost("command-logs/validate", params);
                if (!r.ok) return;

                nlohmann::json data;
                try { data = nlohmann::json::parse(r.body); }
                catch (...) { LOG_ERROR("Invalid validate response: " + r.body); return; }

                if (!data.contains("commands"))
                {
                    LOG_ERROR("Validate response missing 'commands'.");
                    return;
                }

                // Extract command strings and the target player's Steam ID
                nlohmann::json cmds = nlohmann::json::array();
                std::string steam_id;
                for (const auto& item : data["commands"])
                {
                    std::string cmd = item.value("command", "");
                    if (!cmd.empty()) cmds.push_back(cmd);
                    // All commands in one batch share the same target player
                    if (steam_id.empty()) steam_id = item.value("username", "");
                }

                if (!cmds.empty() && !steam_id.empty())
                    ExecuteCommands(cmds, steam_id);
            }
            catch (const std::exception& e) { LOG_ERROR("ValidateAndExecute exception: " + std::string(e.what())); }
            catch (...) { LOG_ERROR("ValidateAndExecute unknown exception"); }

        }).detach();
}

// Fetches the pending command queue from the LeaderOS API.
// Runs in a detached background thread. If the response contains
// command IDs, they are forwarded to ValidateAndExecute.
static void PollQueue()
{
    if (!g_Running) return;
    LOG_DEBUG("Polling queue...");

    std::thread([]()
        {
            try
            {
                auto r = DoGet("command-logs/" + g_Config.ConnectToken + "/queue");
                if (!r.ok) return;

                nlohmann::json data;
                try { data = nlohmann::json::parse(r.body); }
                catch (...) { LOG_ERROR("Invalid queue response."); return; }

                // The API may return the list directly as an array, or wrapped in a key
                nlohmann::json arr;
                if (data.is_array())        arr = data;
                else if (data.contains("array")) arr = data["array"];
                else if (data.contains("data"))  arr = data["data"];
                else                             arr = nlohmann::json::array();

                // Collect IDs — the API may return them as strings or integers
                std::vector<std::string> ids;
                for (const auto& entry : arr)
                {
                    if (!entry.contains("id")) continue;
                    const auto& id = entry["id"];
                    if (id.is_string()) ids.push_back(id.get<std::string>());
                    else if (id.is_number()) ids.push_back(std::to_string(id.get<int>()));
                }

                LOG_DEBUG("Queue: " + std::to_string(ids.size()) + " item(s) found.");
                if (!ids.empty()) ValidateAndExecute(std::move(ids));
            }
            catch (const std::exception& e) { LOG_ERROR("PollQueue exception: " + std::string(e.what())); }
            catch (...) { LOG_ERROR("PollQueue unknown exception"); }

        }).detach();
}

// Self-scheduling timer function. Polls the queue, then schedules itself
// again after FreqMinutes seconds. Stops automatically when g_Running is false.
static void PollLoop()
{
    if (!g_Running) return;
    PollQueue();
    API::Timer::Get().DelayExecute(PollLoop, g_Config.FreqMinutes * 60);
}

// ============================================================
//  Hook: flush pending commands on player join
// ============================================================

static void (*Orig_HandleNewPlayer)(AShooterGameMode*, AShooterPlayerController*, bool) = nullptr;

// Called by the engine whenever a player connects and their character is ready.
// We wait 3 seconds after the event to ensure the character is fully spawned,
// then flush any commands that were queued while the player was offline.
static void Hook_HandleNewPlayer(
    AShooterGameMode* gamemode,
    AShooterPlayerController* new_player,
    bool                      new_arrival)
{
    // Always call the original function first to preserve default game behavior
    if (Orig_HandleNewPlayer)
        Orig_HandleNewPlayer(gamemode, new_player, new_arrival);

    if (new_player == nullptr) return;

    const std::string steam_id =
        std::to_string(ArkApi::IApiUtils::GetSteamIdFromController(new_player));

    // Delay slightly to allow the player's inventory and character to fully initialize
    API::Timer::Get().DelayExecute([steam_id]()
        {
            auto cmds = PendingFlush(steam_id);
            if (cmds.empty()) return;

            LOG_INFO("Flushing " + std::to_string(cmds.size()) + " pending command(s) for " + steam_id + ".");

            // Stagger execution by 1 second per command to avoid rapid-fire issues
            int delay = 0;
            for (const auto& c : cmds)
            {
                std::string cmd = c.get<std::string>();
                API::Timer::Get().DelayExecute([cmd, steam_id]() { RunCommand(cmd, steam_id); }, delay);
                delay += 1;
            }
        }, 3);
}

// ============================================================
//  Console commands (server console only)
// ============================================================
//
//  All handlers guard against in-game player calls by checking
//  p != nullptr. The server console always passes p = nullptr.

// Immediately triggers a queue poll without waiting for the timer
static void Cmd_Poll(APlayerController* p, FString*, bool)
{
    if (p != nullptr) return;
    PollQueue();
    LOG_INFO("Manual poll triggered.");
}

// Toggles verbose debug logging on/off at runtime
static void Cmd_Debug(APlayerController* p, FString*, bool)
{
    if (p != nullptr) return;
    g_Config.DebugMode = !g_Config.DebugMode;
    LOG_INFO(std::string("Debug mode: ") + (g_Config.DebugMode ? "ON" : "OFF"));
}

// Prints the current plugin configuration and state to the log
static void Cmd_Status(APlayerController* p, FString*, bool)
{
    if (p != nullptr) return;
    LOG_INFO("=== LeaderOS Connect Status ===");
    LOG_INFO("URL:         " + g_Config.WebsiteURL);
    LOG_INFO("Token:       " + g_Config.ConnectToken);
    LOG_INFO("Frequency:   " + std::to_string(g_Config.FreqMinutes) + " min");
    LOG_INFO("CheckOnline: " + std::string(g_Config.CheckOnline ? "true" : "false"));
    LOG_INFO("Debug:       " + std::string(g_Config.DebugMode ? "true" : "false"));
    LOG_INFO("Running:     " + std::string(g_Running ? "true" : "false"));
}

// Reloads config.json from disk and immediately triggers a poll if successful
static void Cmd_Reload(APlayerController* p, FString*, bool)
{
    if (p != nullptr) return;
    if (LoadConfig())
    {
        PollQueue();
        LOG_INFO("Config reloaded successfully.");
    }
}

// ============================================================
//  Plugin entry points
// ============================================================

extern "C" __declspec(dllexport) void Plugin_Init()
{
    // Initialize the logger with our plugin name before any log calls
    Log::Get().Init("LeaderOSConnect");
    Log::GetLog()->info("[LeaderOS] Initializing...");

    // Abort early if config is missing or invalid
    if (!LoadConfig()) return;

    g_Running = true;

    // Hook HandleNewPlayer so we can flush offline queues when players join
    ArkApi::GetHooks().SetHook(
        "AShooterGameMode.HandleNewPlayer_Implementation",
        &Hook_HandleNewPlayer,
        reinterpret_cast<LPVOID*>(&Orig_HandleNewPlayer));

    // Register server console commands
    ArkApi::GetCommands().AddConsoleCommand("leaderos.poll", &Cmd_Poll);
    ArkApi::GetCommands().AddConsoleCommand("leaderos.reload", &Cmd_Reload);
    ArkApi::GetCommands().AddConsoleCommand("leaderos.debug", &Cmd_Debug);
    ArkApi::GetCommands().AddConsoleCommand("leaderos.status", &Cmd_Status);

    // Run an immediate poll on startup, then start the recurring timer
    PollQueue();
    API::Timer::Get().DelayExecute(PollLoop, g_Config.FreqMinutes * 60);

    LOG_INFO("Started. Polling every " + std::to_string(g_Config.FreqMinutes) + " minute(s).");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    // Stop the poll loop
    g_Running = false;

    // Remove the player join hook cleanly
    ArkApi::GetHooks().DisableHook(
        "AShooterGameMode.HandleNewPlayer_Implementation",
        &Hook_HandleNewPlayer);

    LOG_INFO("Unloaded.");
}