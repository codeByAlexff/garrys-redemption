-- The Red Dead tab's commands (user request): god mode, heal, lose every bounty, and a
-- teleport to each town on RDR2's map. The server runs them; spawn.lua lists them.

local GR = GR
GR.Commands = GR.Commands or {}
local Commands = GR.Commands

-- RDR2 world positions (metres) from RDR2Mods.com's "List of Teleport Positions in RDR 2"
-- (rdr2mods.com/wiki/pages/list-of-rdr2-teleports), town centres or a main building's door.
-- Horseshoe Overlook there matches the place earlier sessions reached in the game.
-- new_austin: RDR2 has Arthur shot dead on sight there (a sniper, chapters 1 to 6; seen: one
-- hit of 32 health, then dead, seconds after arriving), so only John is sent there.
Commands.TOWNS = {
    { name = "Annesburg", pos = Vector(2904.366, 1248.808, 44.87448) },
    { name = "Armadillo", pos = Vector(-3665.947, -2612.442, -14.08434), new_austin = true },
    { name = "Benedict Point", pos = Vector(-5245.899, -3470.671, -22.09243), new_austin = true },
    { name = "Blackwater", pos = Vector(-798.9842, -1247.722, 43.42519) },
    { name = "Colter", pos = Vector(-1343.558, 2425.952, 307.4015) },
    { name = "Emerald Ranch", pos = Vector(1417.818, 268.0298, 89.61942) },
    { name = "Horseshoe Overlook", pos = Vector(-69.79318, 109.2834, 89.52379) },
    { name = "Lagras", pos = Vector(2105.414, -682.1608, 42.2669) },
    { name = "MacFarlane's Ranch", pos = Vector(-2333.056, -2350.935, 63.20446), new_austin = true },
    { name = "Manzanita Post", pos = Vector(-1962.67, -1614.491, 116.0338) },
    { name = "Rhodes", pos = Vector(1232.205, -1251.088, 73.67763) },
    { name = "Saint Denis", pos = Vector(2209.557, -1346.319, 45.27962) },
    { name = "Sisika Penitentiary", pos = Vector(3348.683, -638.0975, 44.96677) },
    { name = "Strawberry", pos = Vector(-1731.426, -412.8995, 154.8678) },
    { name = "Tumbleweed", pos = Vector(-5517.375, -2936.821, -2.219434), new_austin = true },
    -- Main street: the list's Valentine entry is inside the sheriff's office.
    { name = "Valentine", pos = Vector(-231.3, 703.3, 113.7) },
    { name = "Van Horn", pos = Vector(2983.451, 430.152, 51.17512) },
    { name = "Wapiti Reservation", pos = Vector(448.9348, 2239.867, 248.4422) },
}

-- What the tab lists: { label, console command }.
Commands.LIST = {
    { "God Mode (on / off)", "gr_god" },
    { "Heal Player", "gr_heal" },
    { "Lose All Bounties", "gr_clear_wanted" },
}
for _, town in ipairs(Commands.TOWNS) do
    Commands.LIST[#Commands.LIST + 1] = { "Teleport to " .. town.name, "gr_teleport \"" .. town.name .. "\"" }
end

if CLIENT then return end

local function Host(ply)
    if not IsValid(ply) then ply = player.GetAll()[1] end
    if IsValid(ply) and (game.SinglePlayer() or ply:IsListenServerHost()) then return ply end
end

-- GMod's NOTIFY_* numbers: the names exist only in the client realm.
local GENERIC, ERROR, HINT = 0, 1, 3

local function Note(ply, text, kind)
    if GR.Spawn and GR.Spawn.Note then GR.Spawn.Note(ply, text, kind or GENERIC) end
    GR.Print(text)
end

concommand.Add("gr_god", function(caller)
    local ply = Host(caller)
    if not ply then return end
    if ply:HasGodMode() then
        ply:GodDisable()
        Note(ply, "God mode off", GENERIC)
    else
        ply:GodEnable()
        Note(ply, "God mode on: nothing in either game can hurt you", GENERIC)
    end
end, nil, "Garry's Redemption: switch god mode (GMod's health decides, so RDR2 cannot hurt you either)")

concommand.Add("gr_heal", function(caller)
    local ply = Host(caller)
    if not ply or not ply:Alive() then return end
    ply:SetHealth(ply:GetMaxHealth())
    ply:Extinguish()
    Note(ply, "Healed", GENERIC)
end, nil, "Garry's Redemption: full health")

concommand.Add("gr_teleport", function(caller, _, args)
    local ply = Host(caller)
    if not ply then return end
    local want = string.lower(table.concat(args, " "))
    for _, town in ipairs(Commands.TOWNS) do
        if string.lower(town.name) == want then
            if not (GR.Native and GR.Native.Teleport and GR.Native.Anchored()) then
                Note(ply, "Teleport: RDR2 is not connected, or has the player", ERROR)
                return
            end
            -- Works around the user's "I randomly die after teleporting to a different city".
            if town.new_austin and select(5, GR.Native.HostState()) then
                Note(ply, town.name .. " is in New Austin, where RDR2 has Arthur shot on sight. Only John can go there", ERROR)
                return
            end
            -- The player stops where it is: RDR2 takes it, moves its ped, and GMod anchors to
            -- the new place (a few seconds while it loads).
            if ply:InVehicle() then ply:ExitVehicle() end
            ply:SetMoveType(MOVETYPE_WALK)
            ply:SetLocalVelocity(vector_origin)
            local p = town.pos
            GR.Native.Teleport(p.x, p.y, p.z)
            Note(ply, "Teleporting to " .. town.name .. "...", HINT)
            return
        end
    end
    Note(ply, "Teleport: no town called \"" .. want .. "\"", ERROR)
end, function(cmd)
    local out = {}
    for _, town in ipairs(Commands.TOWNS) do out[#out + 1] = cmd .. " \"" .. town.name .. "\"" end
    return out
end, "Garry's Redemption: teleport to a town on RDR2's map, by name")
