#include <hyprland/src/includes.hpp>

// Hyprland 0.57 replaced CWorkspace (desktop/Workspace.hpp) with
// Workspace::CHLWorkspace and typed workspace IDs. This file builds on both.
#if __has_include(<hyprland/src/workspace/HLWorkspace.hpp>)
#define HYPRGRID_HL057 1
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#else
#define HYPRGRID_HL057 0
#include <hyprland/src/desktop/Workspace.hpp>
#endif

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#if __has_include(<hyprland/src/desktop/view/window/Window.hpp>)
#include <hyprland/src/desktop/view/window/Window.hpp> // 0.57+
#else
#include <hyprland/src/desktop/view/Window.hpp>
#endif
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    // ----- version compatibility ----------------------------------------------

    using GridID = int64_t; // a numbered workspace ID as stored in map.json

    namespace compat {
#if HYPRGRID_HL057
        constexpr auto NO_MODS = Input::HL_MODIFIER_NONE;
#else
        constexpr uint32_t NO_MODS = 0;
#endif

        // Numeric ID of a workspace, or nullopt for named/special workspaces.
        template <typename P>
        std::optional<GridID> idOf(const P& workspace) {
#if HYPRGRID_HL057
            if (const auto id = workspace->numberedID())
                return static_cast<GridID>(*id);
            return std::nullopt;
#else
            if (workspace->m_id > 0)
                return static_cast<GridID>(workspace->m_id);
            return std::nullopt;
#endif
        }

        inline const std::string& nameOf(const PHLWORKSPACE& workspace) {
#if HYPRGRID_HL057
            return workspace->displayName();
#else
            return workspace->m_name;
#endif
        }

        inline PHLWORKSPACE find(GridID id) {
#if HYPRGRID_HL057
            return State::Workspace::state()->query().numbered(Workspace::SWorkspaceNumberedID{static_cast<Workspace::WorkspaceIDContainer>(id)}).run();
#else
            return State::workspaceState()->query().id(static_cast<WORKSPACEID>(id)).run();
#endif
        }

        // Creates an empty numbered workspace. On 0.57 the caller must call ready()
        // once it has been placed/activated.
        inline PHLWORKSPACE create(GridID id, const PHLMONITOR& monitor, const std::string& name) {
#if HYPRGRID_HL057
            return State::Workspace::state()->createNumbered(Workspace::SWorkspaceNumberedID{static_cast<Workspace::WorkspaceIDContainer>(id)}, monitor, name, true);
#else
            return State::workspaceState()->create(static_cast<WORKSPACEID>(id), monitor->m_id, name, true);
#endif
        }

        inline void ready(const PHLWORKSPACE& workspace) {
#if HYPRGRID_HL057
            if (workspace)
                workspace->ready();
#else
            (void)workspace;
#endif
        }

        inline void log(Hyprutils::CLI::eLogLevel level, const std::string& message) {
#if HYPRGRID_HL057
            Log::logger->log(level, "hyprgrid-native", "{}", message);
#else
            Log::logger->log(level, "[hyprgrid-native] {}", message);
#endif
        }
    }

    HANDLE PHANDLE = nullptr;

    constexpr size_t FINGERS         = 3;
    constexpr auto   MODS            = compat::NO_MODS;
    constexpr bool   DISABLE_INHIBIT = false;

    // Hyprland's public gesture API identifies a gesture only by
    // (fingers, direction, mods, scale, disableInhibit). These unusual, exactly
    // representable scales make our own entries unmistakable, so removing them
    // can never delete one of the user's own gesture config lines.
    // The swipe itself ignores the scale.
    constexpr float  GESTURE_TAG  = 1.0F + 1.0F / 1024.0F;
    constexpr float  PROBE_TAG    = 1.0F + 1.0F / 512.0F;

    constexpr GridID FIRST_NEW_ID = 10000;
    constexpr GridID MAX_ID       = INT32_MAX;

    const CHyprColor COLOR_ERROR{1.0, 0.2, 0.2, 1.0};
    const CHyprColor COLOR_WARN{1.0, 0.7, 0.2, 1.0};
    const CHyprColor COLOR_OK{0.2, 1.0, 0.4, 1.0};

    CHyprSignalListener g_reloadListener;

    // plugin { hyprgrid { invert_x = true } }  /  hl.config({ plugin = { hyprgrid = { invert_x = true } } })
    SP<Config::Values::CBoolValue> g_invertX;
    SP<Config::Values::CBoolValue> g_invertY;
    std::string         g_lastRegistrationProblem;

    void                notify(const std::string& message, const CHyprColor& color, float ms) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprgrid-native] " + message, color, ms);
    }

    void warn(const std::string& message) {
        compat::log(Log::WARN, message);
        notify(message, COLOR_WARN, 5000);
    }

    struct Coord {
        int  x = 0;
        int  y = 0;
        auto operator<=>(const Coord&) const = default;
    };

    struct CoordHash {
        size_t operator()(const Coord& c) const {
            const auto packed = (static_cast<uint64_t>(static_cast<uint32_t>(c.x)) << 32) | static_cast<uint32_t>(c.y);
            return std::hash<uint64_t>{}(packed);
        }
    };

    using CellMap = std::unordered_map<Coord, GridID, CoordHash>;

    std::filesystem::path statePath() {
        if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
            return std::filesystem::path{state} / "hypr-grid" / "map.json";
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path{home} / ".local" / "state" / "hypr-grid" / "map.json";
        throw std::runtime_error("HOME and XDG_STATE_HOME are unset");
    }

    std::string label(const Coord& c) {
        return std::format("({},{})", c.x, c.y);
    }

    std::optional<Coord> parseLabel(const std::string& name) {
        // "(x,y)" is what this plugin writes; "grid(x,y)" and "grid(x:y)" are accepted too.
        static const std::regex RE{R"(^(?:grid)?\((-?[0-9]+)[,:](-?[0-9]+)\)$)"};
        std::smatch             match;
        if (!std::regex_match(name, match, RE))
            return std::nullopt;
        try {
            return Coord{std::stoi(match[1].str()), std::stoi(match[2].str())};
        } catch (...) { return std::nullopt; }
    }

    // A workspace may be renamed to "(x,y)" only if it still has its default
    // numeric name or already carries a grid name (possibly a stale one, after
    // its cell was moved). Names the user chose are never touched.
    bool renameable(const std::string& name, GridID id) {
        return name.empty() || name == std::to_string(id) || parseLabel(name).has_value();
    }

    // ----- map.json (shared with hypr-grid.py) --------------------------------
    // Only the "cells" object is rewritten; any other keys are preserved.

    struct SStateFile {
        std::filesystem::path path;
        std::string           text;
        size_t                cellsOpen  = std::string::npos; // index of '{' of "cells"
        size_t                cellsClose = std::string::npos; // index of its matching '}'
    };

    bool locateCells(SStateFile& file) {
        const auto& s   = file.text;
        const auto  key = s.find("\"cells\"");
        if (key == std::string::npos)
            return false;
        auto pos = s.find_first_not_of(" \t\r\n", key + 7);
        if (pos == std::string::npos || s[pos] != ':')
            return false;
        pos = s.find_first_not_of(" \t\r\n", pos + 1);
        if (pos == std::string::npos || s[pos] != '{')
            return false;

        int  depth    = 0;
        bool inString = false;
        for (size_t i = pos; i < s.size(); ++i) {
            const char ch = s[i];
            if (inString) {
                if (ch == '\\')
                    ++i;
                else if (ch == '"')
                    inString = false;
            } else if (ch == '"')
                inString = true;
            else if (ch == '{')
                ++depth;
            else if (ch == '}' && --depth == 0) {
                file.cellsOpen  = pos;
                file.cellsClose = i;
                return true;
            }
        }
        return false;
    }

    SStateFile readStateFile() {
        SStateFile file;
        file.path = statePath();
        if (std::ifstream input{file.path}; input)
            file.text.assign(std::istreambuf_iterator<char>{input}, {});

        if (file.text.find_first_not_of(" \t\r\n") == std::string::npos)
            file.text.clear(); // missing or blank: a fresh file will be written
        else if (!locateCells(file))
            throw std::runtime_error(std::format("{} has no \"cells\" object; refusing to modify it", file.path.string()));
        return file;
    }

    CellMap parseCells(const SStateFile& file) {
        CellMap result;
        if (file.text.empty())
            return result;

        static const std::regex CELL{R"cell("\s*(-?[0-9]+)\s*,\s*(-?[0-9]+)\s*"\s*:\s*([0-9]+))cell"};
        const auto              begin = file.text.cbegin() + static_cast<std::ptrdiff_t>(file.cellsOpen);
        const auto              end   = file.text.cbegin() + static_cast<std::ptrdiff_t>(file.cellsClose);
        for (auto it = std::sregex_iterator(begin, end, CELL); it != std::sregex_iterator{}; ++it) {
            try {
                const Coord c{std::stoi((*it)[1].str()), std::stoi((*it)[2].str())};
                const auto  id = static_cast<GridID>(std::stoll((*it)[3].str()));
                if (id > 0 && id <= MAX_ID)
                    result[c] = id;
            } catch (...) {}
        }
        return result;
    }

    std::string serializeCells(const CellMap& cells) {
        std::vector<std::pair<Coord, GridID>> ordered{cells.begin(), cells.end()};
        std::ranges::sort(ordered, {}, [](const auto& item) { return std::pair{item.first.y, item.first.x}; });

        std::string out = "{";
        for (size_t i = 0; i < ordered.size(); ++i)
            out += std::format("{}\n    \"{},{}\": {}", i == 0 ? "" : ",", ordered[i].first.x, ordered[i].first.y, ordered[i].second);
        out += ordered.empty() ? "}" : "\n  }";
        return out;
    }

    void writeStateFile(SStateFile& file, const CellMap& cells) {
        const auto  cellsText = serializeCells(cells);
        std::string text;
        if (file.text.empty())
            text = "{\n  \"version\": 1,\n  \"cells\": " + cellsText + "\n}\n";
        else
            text = file.text.substr(0, file.cellsOpen) + cellsText + file.text.substr(file.cellsClose + 1);

        std::filesystem::create_directories(file.path.parent_path());
        const auto temporary = file.path.string() + ".native.tmp";
        {
            std::ofstream output{temporary, std::ios::trunc};
            if (!output)
                throw std::runtime_error("cannot write " + temporary);
            output << text;
            output.flush();
            if (!output)
                throw std::runtime_error("cannot finish writing " + temporary);
        }
        std::filesystem::rename(temporary, file.path);

        file.text = std::move(text);
        locateCells(file);
    }

    GridID ensureCell(CellMap& cells, const Coord& coord, bool& dirty) {
        if (const auto found = cells.find(coord); found != cells.end())
            return found->second;

        std::set<GridID> used;
        GridID           next = FIRST_NEW_ID;
        for (const auto& [_, id] : cells) {
            used.insert(id);
            next = std::max(next, id + 1);
        }
        for (const auto& workspace : State::workspaceState()->workspaces()) {
            if (const auto id = compat::idOf(workspace)) {
                used.insert(*id);
                next = std::max(next, *id + 1);
            }
        }
        while (used.contains(next) && next < MAX_ID)
            ++next;
        if (next <= 0 || next >= MAX_ID)
            throw std::runtime_error("no numeric workspace ID is available");

        cells[coord] = next;
        dirty        = true;
        return next;
    }

    // Which grid cell is this workspace? The map (by workspace ID) is authoritative;
    // the "(x,y)" name is only a fallback, because workspaces recreated after a
    // restart get their numeric ID as name until something renames them.
    std::expected<Coord, std::string> resolveOrigin(CellMap& cells, const PHLWORKSPACE& workspace, bool& dirty) {
        const auto id = compat::idOf(workspace);
        if (!id)
            return std::unexpected(std::format("workspace '{}' is a named or special workspace; only numbered workspaces can be grid cells", compat::nameOf(workspace)));

        for (const auto& [coord, cellID] : cells) {
            if (cellID == *id)
                return coord;
        }

        // Ordinary numbered workspaces stay outside the grid unless adopted.
        const auto coord = parseLabel(compat::nameOf(workspace));
        if (!coord)
            return std::unexpected(std::format("workspace '{}' is outside the grid. Jump in with SUPER+G or: hyprctl hyprgrid goto 0,0", compat::nameOf(workspace)));

        if (const auto taken = cells.find(*coord); taken != cells.end()) {
            const auto owner = compat::find(taken->second);
            if (owner && owner != workspace)
                return std::unexpected(
                    std::format("workspace '{}' is named like cell {}, but the map gives that cell to workspace {}", compat::nameOf(workspace), label(*coord), taken->second));
        }

        cells[*coord] = *id;
        dirty         = true;
        return *coord;
    }

    // ----- the gesture ---------------------------------------------------------

    class CGridSwipeGesture final : public ITrackpadGesture {
      public:
        void begin(const STrackpadGestureBegin& event) override {
            ITrackpadGesture::begin(event);
            reset();

            if (!event.swipe)
                return;
            if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
                return;
            if (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress())
                return;

            // Hyprland passes the detected direction of this swipe here (not the
            // configured TRACKPAD_GESTURE_DIR_SWIPE), locked after ~5 units of travel.
            Coord step;
            switch (event.direction) {
                case TRACKPAD_GESTURE_DIR_RIGHT: step = {1, 0}; break;
                case TRACKPAD_GESTURE_DIR_LEFT: step = {-1, 0}; break;
                case TRACKPAD_GESTURE_DIR_UP: step = {0, 1}; break;
                case TRACKPAD_GESTURE_DIR_DOWN: step = {0, -1}; break;
                default: return;
            }
            // plugin:hyprgrid:invert_x / invert_y flip which cell a swipe leads to.
            // The animation always follows the fingers either way.
            if (g_invertX && g_invertX->value())
                step.x = -step.x;
            if (g_invertY && g_invertY->value())
                step.y = -step.y;

            const auto monitor = Desktop::focusState()->monitor();
            if (!monitor || !monitor->m_activeWorkspace)
                return;
            const auto origin = monitor->m_activeWorkspace;

            try {
                auto file  = readStateFile();
                auto cells = parseCells(file);
                bool dirty = false;

                const auto originCoord = resolveOrigin(cells, origin, dirty);
                if (!originCoord) {
                    warn(originCoord.error());
                    return;
                }

                m_targetCoord = Coord{originCoord->x + step.x, originCoord->y + step.y};
                m_targetID    = ensureCell(cells, m_targetCoord, dirty);
                if (dirty)
                    writeStateFile(file, cells);

                // An existing target is slid in with its windows. A new cell has no
                // windows, so its workspace is only created if the swipe commits.
                if (const auto target = compat::find(m_targetID)) {
                    if (target == origin)
                        return;
                    if (target->m_monitor.lock() != monitor) {
                        warn(std::format("cell {} (workspace {}) is on another monitor", label(m_targetCoord), m_targetID));
                        return;
                    }
                    if (!renameable(compat::nameOf(target), m_targetID)) {
                        warn(std::format("workspace {} is named '{}'; refusing to reuse it for cell {}", m_targetID, compat::nameOf(target), label(m_targetCoord)));
                        return;
                    }
                    if (compat::nameOf(target) != label(m_targetCoord))
                        target->rename(label(m_targetCoord));
                    m_target = target;
                }

                const auto originID = compat::idOf(origin);
                if (originID && compat::nameOf(origin) != label(*originCoord) && renameable(compat::nameOf(origin), *originID))
                    origin->rename(label(*originCoord));
            } catch (const std::exception& error) {
                warn(error.what());
                return;
            }

            static auto PWORKSPACEGAP  = CConfigValue<Config::INTEGER>("general:gaps_workspaces");
            static auto PSWIPEDISTANCE = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");

            m_monitor  = monitor;
            m_origin   = origin;
            m_vertical = step.y != 0;
            // Content follows the fingers: a right/down swipe moves it toward +x/+y on screen.
            m_sign           = (event.direction == TRACKPAD_GESTURE_DIR_RIGHT || event.direction == TRACKPAD_GESTURE_DIR_DOWN) ? 1.0 : -1.0;
            m_visualDistance = (m_vertical ? monitor->m_size.y : monitor->m_size.x) + static_cast<double>(*PWORKSPACEGAP);
            m_swipeDistance  = std::clamp(static_cast<double>(*PSWIPEDISTANCE), 1.0, static_cast<double>(UINT32_MAX));
            m_targetOffset   = offset(-m_sign * m_visualDistance);

            m_origin->m_forceRendering = true;
            m_origin->m_alpha->setValueAndWarp(1.F);
            m_origin->m_renderOffset->setValueAndWarp(Vector2D{});
            if (m_target) {
                m_target->m_forceRendering = true;
                m_target->m_alpha->setValueAndWarp(1.F);
                m_target->m_renderOffset->setValueAndWarp(m_targetOffset);
            }
            m_active = true;
        }

        void update(const STrackpadGestureUpdate& event) override {
            if (!m_active || !event.swipe)
                return;

            const auto monitor = m_monitor.lock();
            if (!monitor || monitor->m_activeWorkspace != m_origin || (m_target && m_target->m_monitor.lock() != monitor)) {
                reset(); // monitor vanished or a workspace changed under us
                return;
            }

            const auto raw      = m_vertical ? event.swipe->delta.y : event.swipe->delta.x;
            const auto directed = raw * m_sign;
            m_progress          = std::clamp(m_progress + directed, 0.0, m_swipeDistance);
            m_averageSpeed      = (m_averageSpeed * m_speedPoints + std::abs(directed)) / (m_speedPoints + 1);
            ++m_speedPoints;

            const auto moved = m_sign * m_visualDistance * (m_progress / m_swipeDistance);
            m_targetOffset   = offset(moved - m_sign * m_visualDistance);
            m_origin->m_renderOffset->setValueAndWarp(offset(moved));
            m_origin->updateWindowDecos();
            if (m_target) {
                m_target->m_renderOffset->setValueAndWarp(m_targetOffset);
                m_target->updateWindowDecos();
            }
            g_pHyprRenderer->damageMonitor(monitor);
        }

        void end(const STrackpadGestureEnd& event) override {
            if (!m_active)
                return;

            const auto monitor = m_monitor.lock();
            if (!monitor || monitor->m_activeWorkspace != m_origin || (m_target && m_target->m_monitor.lock() != monitor)) {
                reset();
                return;
            }

            static auto PCANCELRATIO = CConfigValue<Config::FLOAT>("gestures:workspace_swipe_cancel_ratio");
            static auto PMINSPEED    = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_min_speed_to_force");
            const bool  forced       = *PMINSPEED != 0 && m_averageSpeed >= static_cast<double>(*PMINSPEED);
            bool        commit =
                event.swipe && !event.swipe->cancelled && m_progress >= 2.0 && (m_progress >= m_swipeDistance * static_cast<double>(*PCANCELRATIO) || forced);

            PHLWORKSPACE target = m_target;
            bool         created = false;
            if (commit && !target) {
                // Re-check: something may have created the workspace mid-swipe.
                target = compat::find(m_targetID);
                if (!target) {
                    target  = compat::create(m_targetID, monitor, label(m_targetCoord));
                    created = !!target;
                }
                if (!target || target->m_monitor.lock() != monitor) {
                    warn(std::format("could not open cell {} (workspace {}) on this monitor", label(m_targetCoord), m_targetID));
                    if (created)
                        compat::ready(target);
                    target = nullptr;
                    commit = false;
                }
            }

            if (commit) {
                const auto originOffset = m_origin->m_renderOffset->value();
                monitor->changeWorkspace(target);
                if (created)
                    compat::ready(target);
                // changeWorkspace() restarts the configured workspace animation;
                // continue from where the fingers left both workspaces instead.
                target->m_renderOffset->setValue(m_targetOffset);
                *target->m_renderOffset = Vector2D{};
                target->m_alpha->setValueAndWarp(1.F);
                m_origin->m_renderOffset->setValue(originOffset);
                *m_origin->m_renderOffset = offset(m_sign * m_visualDistance);
                m_origin->m_alpha->setValueAndWarp(1.F);
                g_pInputManager->unconstrainMouse();
            } else {
                *m_origin->m_renderOffset = Vector2D{};
                if (m_target)
                    *m_target->m_renderOffset = offset(-m_sign * m_visualDistance);
            }

            m_origin->m_forceRendering = false;
            if (m_target)
                m_target->m_forceRendering = false;
            g_pHyprRenderer->damageMonitor(monitor);
            g_pInputManager->refocus();
            reset(false);
        }

      private:
        Vector2D offset(double amount) const {
            return m_vertical ? Vector2D{0.0, amount} : Vector2D{amount, 0.0};
        }

        void reset(bool clearOffsets = true) {
            if (clearOffsets) {
                if (m_origin) {
                    m_origin->m_forceRendering = false;
                    m_origin->m_renderOffset->setValueAndWarp(Vector2D{});
                }
                if (m_target) {
                    m_target->m_forceRendering = false;
                    m_target->m_renderOffset->setValueAndWarp(Vector2D{});
                }
                if (const auto monitor = m_monitor.lock(); monitor && (m_origin || m_target))
                    g_pHyprRenderer->damageMonitor(monitor);
            }
            m_active         = false;
            m_origin         = nullptr;
            m_target         = nullptr;
            m_monitor        = {};
            m_targetID       = 0;
            m_targetCoord    = {};
            m_targetOffset   = {};
            m_vertical       = false;
            m_sign           = 1;
            m_progress       = 0;
            m_visualDistance = 1;
            m_swipeDistance  = 1;
            m_averageSpeed   = 0;
            m_speedPoints    = 0;
        }

        bool          m_active         = false;
        bool          m_vertical       = false;
        double        m_sign           = 1;
        double        m_progress       = 0;
        double        m_visualDistance = 1;
        double        m_swipeDistance  = 1;
        double        m_averageSpeed   = 0;
        int           m_speedPoints    = 0;
        GridID        m_targetID       = 0;
        Coord         m_targetCoord;
        Vector2D      m_targetOffset;
        PHLWORKSPACE  m_origin;
        PHLWORKSPACE  m_target; // null while the target cell has no workspace yet
        PHLMONITORREF m_monitor;
    };

    // ----- registration --------------------------------------------------------

    // Does nothing; only used to ask Hyprland whether an existing gesture would shadow ours.
    class CProbeGesture final : public ITrackpadGesture {
      public:
        void update(const STrackpadGestureUpdate&) override {}
        void end(const STrackpadGestureEnd&) override {}
    };

    // Config gestures are matched before ours, so e.g. a 3-finger horizontal
    // workspace gesture silently swallows those swipes. Returns the shadowed directions.
    std::vector<std::string> shadowedDirections() {
        std::vector<std::string> shadowed;
        for (const auto direction : {TRACKPAD_GESTURE_DIR_LEFT, TRACKPAD_GESTURE_DIR_RIGHT, TRACKPAD_GESTURE_DIR_UP, TRACKPAD_GESTURE_DIR_DOWN}) {
            const auto probe = g_pTrackpadGestures->addGesture(makeUnique<CProbeGesture>(), FINGERS, direction, MODS, PROBE_TAG, DISABLE_INHIBIT);
            if (probe)
                (void)g_pTrackpadGestures->removeGesture(FINGERS, direction, MODS, PROBE_TAG, DISABLE_INHIBIT);
            else
                shadowed.emplace_back(g_pTrackpadGestures->stringForDir(direction));
        }
        return shadowed;
    }

    // Every config reload (including the one Hyprland runs right after loading
    // any plugin) calls CTrackpadGestures::clearGestures(), which deletes our
    // gesture too. So this runs at init and again after every reload.
    void registerGesture() {
        if (!g_pTrackpadGestures)
            return;

        // Normally already gone; drop it in case a reload did not clear gestures.
        (void)g_pTrackpadGestures->removeGesture(FINGERS, TRACKPAD_GESTURE_DIR_SWIPE, MODS, GESTURE_TAG, DISABLE_INHIBIT);

        const auto  shadowed = shadowedDirections();
        const auto  added    = g_pTrackpadGestures->addGesture(makeUnique<CGridSwipeGesture>(), FINGERS, TRACKPAD_GESTURE_DIR_SWIPE, MODS, GESTURE_TAG, DISABLE_INHIBIT);

        std::string problem;
        if (!added)
            problem = std::format("3-finger swipe not registered ({}). Remove the 3-finger gestures from your Hyprland config.", added.error());
        else if (!shadowed.empty()) {
            std::string list;
            for (const auto& d : shadowed)
                list += (list.empty() ? "" : ", ") + d;
            problem = std::format("3-finger gestures in your Hyprland config take priority for these swipes: {}. Remove them.", list);
        }

        if (problem.empty()) {
            g_lastRegistrationProblem.clear();
            return;
        }

        compat::log(Log::ERR, problem);
        if (problem != g_lastRegistrationProblem)
            notify(problem, COLOR_ERROR, 10000);
        g_lastRegistrationProblem = problem;
    }

    // ----- hyprctl hyprgrid ... (moving whole cells) ----------------------------

    const std::string USAGE = "usage:\n"
                              "  hyprctl hyprgrid shift left|right|up|down   move the current workspace one cell (swaps with a neighbour)\n"
                              "  hyprctl hyprgrid move X,Y                    move the current workspace to cell X,Y (swaps if taken)\n"
                              "  hyprctl hyprgrid swap X,Y X,Y                swap two cells, or move one into an empty cell\n"
                              "  hyprctl hyprgrid list                        show the grid and workspaces outside it\n"
                              "  hyprctl hyprgrid adopt [X,Y]                 add the current workspace to the grid (next free cell, or X,Y)\n"
                              "  hyprctl hyprgrid adopt-all                   add every numbered workspace that has windows to the grid\n"
                              "  hyprctl hyprgrid goto X,Y                    go to cell X,Y (creates it if new)\n"
                              "  hyprctl hyprgrid release [X,Y]               take the current cell (or X,Y) out of the grid\n"
                              "  hyprctl hyprgrid send X,Y [follow] [window=0xADDR]   move the focused (or given) window to cell X,Y (follow: go with it)\n"
                              "  hyprctl hyprgrid send left|right|up|down [follow]   ...or to a neighbouring cell\n";

    std::optional<Coord> parseCoord(std::string text) {
        if (text.size() >= 2 && text.front() == '(' && text.back() == ')')
            text = text.substr(1, text.size() - 2);
        static const std::regex RE{R"(^(-?[0-9]+),(-?[0-9]+)$)"};
        std::smatch             match;
        if (!std::regex_match(text, match, RE))
            return std::nullopt;
        try {
            return Coord{std::stoi(match[1].str()), std::stoi(match[2].str())};
        } catch (...) { return std::nullopt; }
    }

    void relabel(GridID id, const Coord& coord) {
        const auto workspace = compat::find(id);
        if (workspace && compat::nameOf(workspace) != label(coord) && renameable(compat::nameOf(workspace), id))
            workspace->rename(label(coord));
    }

    Coord currentCell() {
        const auto monitor = Desktop::focusState()->monitor();
        if (!monitor || !monitor->m_activeWorkspace)
            throw std::runtime_error("no focused workspace");
        auto       file   = readStateFile();
        auto       cells  = parseCells(file);
        bool       dirty  = false;
        const auto coord  = resolveOrigin(cells, monitor->m_activeWorkspace, dirty);
        if (!coord)
            throw std::runtime_error(coord.error());
        if (dirty)
            writeStateFile(file, cells);
        return *coord;
    }

    // Puts the workspace of cell `from` (with its windows) at `to`; whatever
    // was at `to` goes to `from`. Only the map and the workspace names change,
    // so nothing on screen moves.
    std::string moveCell(const Coord& from, const Coord& to) {
        if (from == to)
            return std::format("{} is already there", label(from));

        auto       file  = readStateFile();
        auto       cells = parseCells(file);
        const auto a     = cells.find(from);
        if (a == cells.end())
            throw std::runtime_error(std::format("cell {} is empty", label(from)));
        const GridID          idFrom = a->second;
        std::optional<GridID> idTo;
        if (const auto b = cells.find(to); b != cells.end())
            idTo = b->second;

        cells[to] = idFrom;
        if (idTo)
            cells[from] = *idTo;
        else
            cells.erase(from);
        writeStateFile(file, cells);

        relabel(idFrom, to);
        if (idTo)
            relabel(*idTo, from);

        const auto message = idTo ? std::format("swapped {} and {}", label(from), label(to)) : std::format("moved {} to {}", label(from), label(to));
        notify(message, COLOR_OK, 1500);
        return message;
    }

    std::vector<GridID> workspacesOutsideGrid(const CellMap& cells, bool onlyWithWindows);

    std::string listCells() {
        auto       file  = readStateFile();
        const auto cells = parseCells(file);
        if (cells.empty())
            return "the grid is empty\n";

        std::vector<std::pair<Coord, GridID>> ordered{cells.begin(), cells.end()};
        std::ranges::sort(ordered, {}, [](const auto& item) { return std::pair{-item.first.y, item.first.x}; });

        const auto  monitor = Desktop::focusState()->monitor();
        const auto  active  = monitor ? monitor->m_activeWorkspace : nullptr;
        std::string out;
        for (const auto& [coord, id] : ordered) {
            const auto workspace = compat::find(id);
            out += std::format("{:>10}  workspace {:<6} {}{}\n", label(coord), id, workspace ? std::format("{} windows", workspace->getWindowCount()) : "not open",
                               workspace && workspace == active ? "  <- current" : "");
        }

        const auto outside = workspacesOutsideGrid(cells, false);
        if (!outside.empty()) {
            out += "\nnot in the grid (add with: hyprctl hyprgrid adopt-all):\n";
            for (const auto id : outside) {
                const auto workspace = compat::find(id);
                out += std::format("{:>10}  workspace {:<6} {} windows{}\n", "-", id, workspace->getWindowCount(), workspace == active ? "  <- current" : "");
            }
        }
        return out;
    }

    // ----- adopting ordinary numbered workspaces into the grid -----------------
    // The workspace keeps its ID and its windows; it just gets a cell in the map.

    // Open numbered workspaces that are not grid cells, lowest ID first.
    std::vector<GridID> workspacesOutsideGrid(const CellMap& cells, bool onlyWithWindows) {
        std::set<GridID> inGrid;
        for (const auto& [_, id] : cells)
            inGrid.insert(id);

        std::vector<GridID> result;
        for (const auto& ref : State::workspaceState()->workspaces()) {
            const auto id = compat::idOf(ref);
            if (!id || inGrid.contains(*id))
                continue;
            const auto workspace = compat::find(*id);
            if (!workspace || (onlyWithWindows && workspace->getWindowCount() == 0))
                continue;
            result.push_back(*id);
        }
        std::ranges::sort(result);
        result.erase(std::ranges::unique(result).begin(), result.end());
        return result;
    }

    // First free cell on row 0, going right from x = 0.
    Coord nextFreeCell(const CellMap& cells) {
        for (int x = 0;; ++x) {
            if (!cells.contains(Coord{x, 0}))
                return Coord{x, 0};
        }
    }

    std::string adoptCurrent(std::optional<Coord> at) {
        const auto monitor = Desktop::focusState()->monitor();
        if (!monitor || !monitor->m_activeWorkspace)
            throw std::runtime_error("no focused workspace");
        const auto workspace = monitor->m_activeWorkspace;
        const auto id        = compat::idOf(workspace);
        if (!id)
            throw std::runtime_error(std::format("workspace '{}' is a named or special workspace; only numbered workspaces can be grid cells", compat::nameOf(workspace)));

        auto file  = readStateFile();
        auto cells = parseCells(file);
        for (const auto& [coord, cellID] : cells) {
            if (cellID == *id)
                return std::format("workspace {} is already cell {}", *id, label(coord));
        }

        const auto coord = at ? *at : nextFreeCell(cells);
        if (cells.contains(coord))
            throw std::runtime_error(std::format("cell {} is taken; pick an empty one (see: hyprctl hyprgrid list)", label(coord)));

        cells[coord] = *id;
        writeStateFile(file, cells);
        relabel(*id, coord);

        const auto message = std::format("workspace {} is now cell {}", *id, label(coord));
        notify(message, COLOR_OK, 2000);
        return message;
    }

    std::string adoptAll() {
        auto       file  = readStateFile();
        auto       cells = parseCells(file);
        const auto ids   = workspacesOutsideGrid(cells, true);
        if (ids.empty())
            return "every numbered workspace with windows is already in the grid";

        std::vector<std::pair<GridID, Coord>> placed;
        for (const auto id : ids) {
            const auto coord = nextFreeCell(cells);
            cells[coord]     = id;
            placed.emplace_back(id, coord);
        }
        writeStateFile(file, cells);

        std::string message = std::format("added {} workspace{} to the grid:", placed.size(), placed.size() == 1 ? "" : "s");
        for (const auto& [id, coord] : placed) {
            relabel(id, coord);
            message += std::format("\n  workspace {} -> {}", id, label(coord));
        }
        notify(std::format("added {} workspace{} to the grid", placed.size(), placed.size() == 1 ? "" : "s"), COLOR_OK, 2500);
        return message;
    }

    // ----- jumping to a cell / leaving the grid --------------------------------

    // The workspace of a cell, created (and added to the map) if needed. On
    // Hyprland 0.57 a new workspace must be finished with ready() once it has
    // been placed; the destructor does that, so keep this alive until then.
    struct SOpenedCell {
        PHLWORKSPACE workspace;
        bool         created = false;

        SOpenedCell()                              = default;
        SOpenedCell(const SOpenedCell&)            = delete;
        SOpenedCell& operator=(const SOpenedCell&) = delete;
        ~SOpenedCell() {
            if (created)
                compat::ready(workspace);
        }
    };

    void openCell(const Coord& coord, const PHLMONITOR& monitor, SOpenedCell& out) {
        auto       file  = readStateFile();
        auto       cells = parseCells(file);
        bool       dirty = false;
        const auto id    = ensureCell(cells, coord, dirty);
        if (dirty)
            writeStateFile(file, cells);

        out.workspace = compat::find(id);
        if (!out.workspace) {
            out.workspace = compat::create(id, monitor, label(coord));
            out.created   = !!out.workspace;
        }
        if (!out.workspace)
            throw std::runtime_error(std::format("could not create workspace {} for cell {}", id, label(coord)));
        if (!renameable(compat::nameOf(out.workspace), id))
            throw std::runtime_error(std::format("workspace {} is named '{}'; refusing to use it as cell {}", id, compat::nameOf(out.workspace), label(coord)));
        if (!out.workspace->m_monitor.lock())
            throw std::runtime_error(std::format("cell {} has no monitor", label(coord)));
        relabel(id, coord);
    }

    std::string gotoCell(const Coord& coord) {
        const auto monitor = Desktop::focusState()->monitor();
        if (!monitor)
            throw std::runtime_error("no focused monitor");

        SOpenedCell cell;
        openCell(coord, monitor, cell);

        // A cell normally lives on the focused monitor; if a workspace rule put
        // it elsewhere, show it there.
        const auto target = cell.workspace->m_monitor.lock();
        if (target->m_activeWorkspace == cell.workspace)
            return std::format("already on {}", label(coord));

        target->changeWorkspace(cell.workspace);
        g_pInputManager->refocus();
        return std::format("went to {}", label(coord));
    }

    // Moves the focused window to a cell, using the same action as Hyprland's
    // own move-to-workspace keybinds. With follow, you go along with it.
    // Finds a window by the address hyprctl prints ("0x55d0..."), so a script
    // can name the window it saw focused before opening a menu.
    PHLWINDOW windowByAddress(std::string address) {
        if (address.starts_with("address:"))
            address = address.substr(8);
        uintptr_t value = 0;
        try {
            value = static_cast<uintptr_t>(std::stoull(address, nullptr, 16));
        } catch (...) { throw std::runtime_error("bad window address '" + address + "'"); }
        for (const auto& window : Desktop::windowState()->windows()) {
            if (window && reinterpret_cast<uintptr_t>(window.get()) == value)
                return window;
        }
        throw std::runtime_error("that window no longer exists");
    }

    std::string sendWindow(const Coord& coord, bool follow, PHLWINDOW window) {
        if (!window)
            window = Desktop::focusState()->window();
        const auto monitor = Desktop::focusState()->monitor();
        if (!window)
            throw std::runtime_error("no focused window to send");
        if (!monitor)
            throw std::runtime_error("no focused monitor");

        SOpenedCell cell;
        openCell(coord, monitor, cell);
        if (window->m_workspace == cell.workspace)
            return std::format("the window is already in {}", label(coord));

        const auto result = Config::Actions::moveToWorkspace(cell.workspace, !follow, window);
        if (!result)
            throw std::runtime_error(result.error().message);
        return std::format("sent the window to {}", label(coord));
    }

    std::optional<Coord> parseStep(const std::string& direction) {
        if (direction == "left")
            return Coord{-1, 0};
        if (direction == "right")
            return Coord{1, 0};
        if (direction == "up")
            return Coord{0, 1};
        if (direction == "down")
            return Coord{0, -1};
        return std::nullopt;
    }

    // Takes a cell out of the grid. Its workspace (and windows) stays as an
    // ordinary numbered workspace and gets its number back as name.
    std::string releaseCell(std::optional<Coord> at) {
        const Coord coord = at ? *at : currentCell();

        auto        file  = readStateFile();
        auto        cells = parseCells(file);
        const auto  it    = cells.find(coord);
        if (it == cells.end())
            throw std::runtime_error(std::format("cell {} is empty", label(coord)));
        const GridID id = it->second;
        cells.erase(it);
        writeStateFile(file, cells);

        if (const auto workspace = compat::find(id); workspace && parseLabel(compat::nameOf(workspace)))
            workspace->rename(std::to_string(id));

        const auto message = std::format("cell {} removed from the grid; workspace {} is a normal workspace again", label(coord), id);
        notify(message, COLOR_OK, 2500);
        return message;
    }

    std::string runCommand(const std::string& request) {
        std::vector<std::string> args;
        std::string              windowArg; // window=0x... (optional, for send)
        std::istringstream       in{request};
        for (std::string word; in >> word;) {
            if (word.starts_with("window="))
                windowArg = word.substr(7);
            else
                args.push_back(word);
        }

        try {
            if (args.size() < 2 || args[0] != "hyprgrid")
                return USAGE;
            const auto& verb = args[1];

            if (verb == "list" && args.size() == 2)
                return listCells();

            if (verb == "adopt" && (args.size() == 2 || args.size() == 3)) {
                std::optional<Coord> at;
                if (args.size() == 3 && !(at = parseCoord(args[2])))
                    return "error: cell must look like 2,1 or (2,1)\n";
                return adoptCurrent(at) + "\n";
            }

            if (verb == "adopt-all" && args.size() == 2)
                return adoptAll() + "\n";

            // goto X,Y  |  goto (X,Y)  |  goto X Y
            if (verb == "goto" && (args.size() == 3 || args.size() == 4)) {
                const auto coord = args.size() == 3 ? parseCoord(args[2]) : parseCoord(args[2] + "," + args[3]);
                if (!coord)
                    return "error: cell must look like 2,1 or 2 1\n";
                return gotoCell(*coord) + "\n";
            }

            // send X,Y [follow]  |  send left|right|up|down [follow]
            if (verb == "send" && (args.size() == 3 || (args.size() == 4 && args[3] == "follow"))) {
                const bool      follow = args.size() == 4;
                const PHLWINDOW window = windowArg.empty() ? nullptr : windowByAddress(windowArg);
                if (const auto step = parseStep(args[2])) {
                    const auto from = currentCell();
                    return sendWindow(Coord{from.x + step->x, from.y + step->y}, follow, window) + "\n";
                }
                const auto coord = parseCoord(args[2]);
                if (!coord)
                    return "error: give a cell like 2,1 or a direction (left/right/up/down)\n";
                return sendWindow(*coord, follow, window) + "\n";
            }

            if (verb == "release" && (args.size() == 2 || args.size() == 3)) {
                std::optional<Coord> at;
                if (args.size() == 3 && !(at = parseCoord(args[2])))
                    return "error: cell must look like 2,1 or (2,1)\n";
                return releaseCell(at) + "\n";
            }

            if (verb == "shift" && args.size() == 3) {
                Coord step;
                if (args[2] == "left")
                    step = {-1, 0};
                else if (args[2] == "right")
                    step = {1, 0};
                else if (args[2] == "up")
                    step = {0, 1};
                else if (args[2] == "down")
                    step = {0, -1};
                else
                    return USAGE;
                const auto from = currentCell();
                return moveCell(from, Coord{from.x + step.x, from.y + step.y}) + "\n";
            }

            if (verb == "move" && args.size() == 3) {
                const auto to = parseCoord(args[2]);
                if (!to)
                    return "error: cell must look like 2,1 or (2,1)\n";
                return moveCell(currentCell(), *to) + "\n";
            }

            if (verb == "swap" && args.size() == 4) {
                const auto a = parseCoord(args[2]);
                const auto b = parseCoord(args[3]);
                if (!a || !b)
                    return "error: cells must look like 2,1 or (2,1)\n";
                return moveCell(*a, *b) + "\n";
            }

            return USAGE;
        } catch (const std::exception& error) {
            warn(error.what());
            return std::string{"error: "} + error.what() + "\n";
        }
    }

    void registerCommand() {
#if HYPRGRID_HL057
        HyprlandAPI::registerHyprCtlCommand(PHANDLE,
                                            IPC::Socket1::SCommand{
                                                .name    = "hyprgrid",
                                                .match   = IPC::Socket1::COMMAND_MATCH_PREFIX,
                                                .handler = [](const IPC::Socket1::SRequest& request) { return IPC::Socket1::SResponse{runCommand(request.command)}; },
                                            });
#else
        HyprlandAPI::registerHyprCtlCommand(PHANDLE,
                                            SHyprCtlCommand{
                                                .name  = "hyprgrid",
                                                .exact = false,
                                                .fn    = [](eHyprCtlOutputFormat, std::string request) { return runCommand(request); },
                                            });
#endif
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // Compare the strings, not the pointers: the two functions return different buffers.
    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        notify(std::format("Hyprland/header version mismatch (running {}, built against {}). Rebuild the plugin.", HASH, CLIENT_HASH), COLOR_ERROR, 10000);
        throw std::runtime_error("hyprgrid-native: version mismatch");
    }

    // Nothing below may throw: Hyprland does not call PLUGIN_EXIT for a plugin
    // whose init threw, so a listener registered here would be left dangling.
    // Config values can only be added during init.
    g_invertX = makeShared<Config::Values::CBoolValue>("plugin:hyprgrid:invert_x", "swipe right goes to x-1 instead of x+1", false);
    g_invertY = makeShared<Config::Values::CBoolValue>("plugin:hyprgrid:invert_y", "swipe up goes to y-1 instead of y+1", false);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_invertX);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_invertY);

    g_reloadListener = Event::bus()->m_events.config.reloaded.listen([] { registerGesture(); });
    registerGesture();
    registerCommand(); // Hyprland unregisters it automatically on unload

    if (g_lastRegistrationProblem.empty())
        notify("loaded: 3-finger swipe moves around the workspace grid", COLOR_OK, 4000);

    return {"hyprgrid-native", "Native cancellable 2D workspace-grid gestures", "OpenAI with Kawsaj Alannaq", "0.7.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_reloadListener.reset();
    if (g_pTrackpadGestures)
        (void)g_pTrackpadGestures->removeGesture(FINGERS, TRACKPAD_GESTURE_DIR_SWIPE, MODS, GESTURE_TAG, DISABLE_INHIBIT);
}
