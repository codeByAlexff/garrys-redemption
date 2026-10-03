-- gr_probe: measures whether GMod keeps running when it is unfocused, minimised, hidden
-- or paused, and which hooks still fire in each case.
--
-- GMod has to tick the bridge every frame while RDR2 has the focus, so this decides
-- which hook the bridge is ticked from and whether single player (which pauses with the
-- menu open) can be used at all. Turn it on with `gr_probe 1`; it writes one line a
-- second to garrysmod/data/gr_probe.txt.

local GR = GR
GR.Probe = GR.Probe or {}
local Probe = GR.Probe

local LOG = "gr_probe.txt"
local HOOKS = { "Think", "Tick", "PreRender", "DrawOverlay" }
-- A gap this long between two calls of anything means the whole game stood still.
local STALL_SECONDS = 0.25

local enabled = CreateClientConVar("gr_probe", "0", false, false,
    "Garry's Redemption: 1 = log once a second which hooks are running, to data/gr_probe.txt")

-- Lets a test run cover the paused case with nobody at the keyboard.
local menu_at = CreateClientConVar("gr_probe_menu_at", "0", false, false,
    "Garry's Redemption: N > 0 = the probe opens the game menu N seconds after it starts, for 10 seconds")
local MENU_SECONDS = 10

local running = false
local counts, max_gap, last_call = {}, {}, {}
local last_any, report_at, report_curtime, focus
local began, menu_phase

local function Line(text)
    file.Append(LOG, os.date("%H:%M:%S") .. "  " .. text .. "\n")
end

local function Reset(now)
    for _, name in ipairs(HOOKS) do
        counts[name] = 0
        max_gap[name] = 0
    end
    report_at = now
    report_curtime = CurTime()
end

local function Begin(now)
    running = true
    for _, name in ipairs(HOOKS) do last_call[name] = nil end
    last_any = now
    began = now
    menu_phase = 0
    focus = system.HasFocus()
    Reset(now)
    -- The engine's own unfocused throttle, if this build has the convar.
    local sleep = GetConVar("engine_no_focus_sleep")
    Line(string.format("---- probe on. GMod %s, branch %s, %s, single player %s, max players %d, map %s, focus %s, engine_no_focus_sleep %s",
        VERSIONSTR, BRANCH, jit.arch, tostring(game.SinglePlayer()), game.MaxPlayers(),
        game.GetMap(), tostring(focus), sleep and sleep:GetString() or "(no such convar)"))
    Line("each line: seconds covered | focus | game menu open | CurTime advance | per hook: calls / longest gap in ms")
    GR.Print("probe on, writing garrysmod/data/", LOG)
end

local function Report(now)
    local parts = {}
    for _, name in ipairs(HOOKS) do
        parts[#parts + 1] = string.format("%s %d/%.0f", name, counts[name], max_gap[name] * 1000)
    end
    Line(string.format("%.2fs | focus %d | menu %d | curtime +%.2f | %s",
        now - report_at, focus and 1 or 0, gui.IsGameUIVisible() and 1 or 0,
        CurTime() - report_curtime, table.concat(parts, " | ")))
    Reset(now)
end

local function Seen(name)
    if not enabled:GetBool() then
        if running then
            running = false
            Line("---- probe off")
            GR.Print("probe off.")
        end
        return
    end

    local now = SysTime()
    if not running then Begin(now) end

    -- Nothing at all ran for a while: the process was stalled, not just one hook.
    if now - last_any > STALL_SECONDS then
        Line(string.format("STALL: nothing ran for %.2fs (first hook back: %s)", now - last_any, name))
    end
    last_any = now

    local now_focus = system.HasFocus()
    if now_focus ~= focus then
        focus = now_focus
        Line("focus " .. (focus and "gained" or "lost"))
    end

    local prev = last_call[name]
    if prev and now - prev > max_gap[name] then max_gap[name] = now - prev end
    last_call[name] = now
    counts[name] = counts[name] + 1

    local at = tonumber(menu_at:GetString()) or 0
    if at > 0 then
        if menu_phase == 0 and now - began >= at then
            menu_phase = 1
            Line("opening the game menu")
            gui.ActivateGameUI()
        elseif menu_phase == 1 and now - began >= at + MENU_SECONDS then
            menu_phase = 2
            Line("closing the game menu")
            gui.HideGameUI()
        end
    end

    if now - report_at >= 1 then Report(now) end
end

for _, name in ipairs(HOOKS) do
    hook.Add(name, "GR.Probe", function() Seen(name) end)
end

Probe.log_name = LOG
