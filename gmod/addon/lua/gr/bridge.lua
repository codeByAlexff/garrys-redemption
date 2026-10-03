-- The Lua end of the bridge: loads the binary module, ticks it once per frame, and says
-- what the link is doing.
--
-- The module (garrysmod/lua/bin/gmcl_gr_win64.dll, loaded with require("gr")) puts its
-- functions in GR.Native:
--   GR.Native.Tick()            first in a frame: heartbeat, read RDR2's frame. Returns State().
--   GR.Native.Publish()         last in a frame: send ours
--   GR.Native.Anchor/SetPlayer  see player.lua;  NextKey/Mouse/HostState  see input.lua
--   GR.Native.State()           "closed", "waiting", "connected", "version-mismatch",
--                               "peer-refused" or "refusing"
--   GR.Native.Stats()           table: see PrintStats below
--   GR.Native.SetVerbose(bool)
--   GR.Native.SetKeepActive(bool)

local GR = GR
GR.Bridge = GR.Bridge or {}
local Bridge = GR.Bridge

local MODULE = "gr"

-- Why the bridge is not running, or nil while it is.
Bridge.off_reason = Bridge.off_reason or "the map has not finished loading"
Bridge.last_state = Bridge.last_state or "closed"

local verbose = CreateClientConVar("gr_verbose", "0", true, false,
    "Garry's Redemption: 1 = the module logs frame statistics")

-- Works around GMod dropping to 20 fps whenever it is not the window in front, which is
-- always once RDR2 is (docs/DESIGN.md). The switch is for telling that workaround apart
-- from other problems.
local keep_active = CreateClientConVar("gr_keep_active", "1", true, false,
    "Garry's Redemption: 1 = keep GMod at full speed while RDR2 is connected and in front")

-- GMod as a background task (gmod/module/src/window_fit.h): no window on the desktop while
-- RDR2 runs, and always RDR2's resolution, whatever that is changed to.
local hide_window = CreateClientConVar("gr_hide_window", "1", true, false,
    "Garry's Redemption: 1 = GMod's window is hidden while RDR2 runs (it comes back when RDR2 closes)")
local match_resolution = CreateClientConVar("gr_match_resolution", "1", true, false,
    "Garry's Redemption: 1 = GMod changes its resolution to RDR2's whenever they differ")

local quit_with_rdr2 = CreateClientConVar("gr_quit_with_rdr2", "1", true, false,
    "Garry's Redemption: 1 = Garry's Mod closes when RDR2 closes after the two were linked")

local STATE_TEXT = {
    ["closed"] = "the shared memory could not be opened",
    ["waiting"] = "waiting for RDR2 (start the game and load into story mode)",
    ["connected"] = "connected to RDR2",
    ["version-mismatch"] = "version mismatch: the RDR2 plugin and this module are from different releases",
    ["peer-refused"] = "the RDR2 plugin refuses to run",
    ["refusing"] = "this module refuses to run",
}

local function PrintStats(stats)
    GR.Print(string.format("  protocol %d, shared memory %d bytes", stats.protocol, stats.shm_size))
    GR.Print(string.format("  frames: ours %d, RDR2's %d, torn reads %d",
        stats.guest_frame, stats.host_frame, stats.torn_reads))
    if stats.peer_pid ~= 0 then
        GR.Print(string.format("  RDR2 plugin: pid %d, protocol %d, shared memory %d bytes%s",
            stats.peer_pid, stats.peer_protocol, stats.peer_shm_size,
            stats.peer_reason ~= "none" and (", refusing: " .. stats.peer_reason) or ""))
    end
    GR.Print(string.format("  keep-active %s, engine believes it is %s, %d deactivations swallowed",
        stats.keep_active and "on" or "off", stats.engine_active and "active" or "inactive",
        stats.deactivations_swallowed))
    GR.Print(string.format("  player: %s, input %s, RDR2 %s%s%s, its frame is %.1f ms old",
        stats.anchored and "anchored (GMod has the player)" or "not anchored",
        stats.captured and "captured from RDR2" or "not captured",
        stats.rdr2_focused and "in front" or "not in front",
        stats.rdr2_paused and ", paused" or "", stats.rdr2_owns_player and ", owns the player" or "",
        stats.host_frame_age_ms))
    if stats.anchored then
        GR.Print(string.format("  in RDR2: (%.2f, %.2f, %.2f) m, looking yaw %.1f pitch %.1f; mouse total (%d, %d)",
            stats.rdr2_x, stats.rdr2_y, stats.rdr2_z, stats.rdr2_yaw, stats.rdr2_pitch,
            stats.mouse_x, stats.mouse_y))
    end
end

-- The bridge only ever runs in a game this machine hosts. Joined to someone else's
-- server it stays off: it would be feeding RDR2 a player that server controls.
local function OffReason()
    if not game.SinglePlayer() and not LocalPlayer():IsListenServerHost() then
        return "this is someone else's server. Start a single player game"
    end
    if jit.arch ~= "x64" then
        return "GMod is running as 32-bit. The module needs the 64-bit game"
    end
    if not util.IsBinaryModuleInstalled(MODULE) then
        return "the module is not installed. Expected garrysmod/lua/bin/gmcl_gr_win64.dll"
    end
    return nil
end

-- Runs as a PreRender hook, where returning true would cancel the frame's rendering:
-- every path returns nothing.
function Bridge.Tick()
    local state = GR.Native.Tick()
    if state ~= Bridge.last_state then
        GR.Print("bridge: ", STATE_TEXT[state] or state)
        if state == "version-mismatch" or state == "peer-refused" then
            PrintStats(GR.Native.Stats())
        end
        Bridge.last_state = state
    end
    GR.Player.Frame()
    GR.Overlay.Frame()
    GR.Native.Publish()
end

function Bridge.Start()
    Bridge.off_reason = OffReason()
    if Bridge.off_reason then
        GR.Print("bridge off: ", Bridge.off_reason, ".")
        return
    end

    -- require() raises if the module fails to load, and that must not take the rest of
    -- the addon down with it.
    local ok, err = pcall(require, MODULE)
    if not ok or not GR.Native then
        Bridge.off_reason = "the module failed to load (" .. tostring(err or "it did not register GR.Native") .. ")"
        GR.Print("bridge off: ", Bridge.off_reason, ".")
        return
    end

    GR.Native.SetVerbose(verbose:GetBool())
    GR.Native.SetKeepActive(keep_active:GetBool())
    GR.Native.SetWindowFit(hide_window:GetBool(), match_resolution:GetBool(), quit_with_rdr2:GetBool())
    Bridge.last_state = "closed"

    -- PreRender, not Think: single player pauses whenever the game menu is open, and a
    -- paused game stops calling Think and Tick. The RDR2 plugin would see the heartbeat
    -- stop and drop the link. PreRender keeps running paused, unfocused, minimised and
    -- hidden (measured with gr_probe, see docs/DESIGN.md).
    hook.Add("PreRender", "GR.Bridge.Tick", Bridge.Tick)
    GR.Print("bridge: module loaded.")
end

cvars.AddChangeCallback("gr_verbose", function(_, _, new)
    if GR.Native then GR.Native.SetVerbose(tonumber(new) == 1) end
end, "GR.Bridge")

cvars.AddChangeCallback("gr_keep_active", function(_, _, new)
    if GR.Native then GR.Native.SetKeepActive(tonumber(new) == 1) end
end, "GR.Bridge")

local function WindowFit()
    if GR.Native then
        GR.Native.SetWindowFit(hide_window:GetBool(), match_resolution:GetBool(), quit_with_rdr2:GetBool())
    end
end
cvars.AddChangeCallback("gr_quit_with_rdr2", WindowFit, "GR.Bridge")
cvars.AddChangeCallback("gr_hide_window", WindowFit, "GR.Bridge")
cvars.AddChangeCallback("gr_match_resolution", WindowFit, "GR.Bridge")

concommand.Add("gr_status", function()
    if Bridge.off_reason then
        GR.Print("bridge off: ", Bridge.off_reason, ".")
        return
    end
    local state = GR.Native.State()
    GR.Print("bridge: ", STATE_TEXT[state] or state)
    PrintStats(GR.Native.Stats())
end, nil, "Garry's Redemption: show the state of the bridge to RDR2")

-- LocalPlayer() is NULL until InitPostEntity. The IsValid branch is for a Lua refresh of
-- this file in a running game, when that hook has already been and gone.
if IsValid(LocalPlayer()) then
    Bridge.Start()
else
    hook.Add("InitPostEntity", "GR.Bridge.Start", Bridge.Start)
end
