-- Health and death while linked to RDR2.
--
-- GMod's health is the player's health. RDR2's damage to the hidden ped comes off it
-- (weapons.lua), and the ped is kept alive while GMod drives. The two are tied at the
-- handovers (protocol 17): when RDR2 takes the player (a horse, a scripted scene, F9) its ped
-- gets GMod's health, and when GMod takes the player back it gets the ped's. Every session
-- (either game started, or the plugin reloaded) starts at full health.
--
-- A dead player hears GMod's death beep and comes back by itself near where it died
-- (terrain.lua, RespawnPos), a few seconds later; a click brings it back sooner, as in GMod.

local GR = GR
GR.Health = GR.Health or {}
local Health = GR.Health

local function Anchored()
    return GR.Native and GR.Native.Anchored and GR.Native.Anchored()
end

if SERVER then
    local delay = CreateConVar("gr_respawn_delay", "3", FCVAR_ARCHIVE,
        "Garry's Redemption: seconds after death before the player comes back by itself (0: only by a click)")

    hook.Add("Tick", "GR.Health", function()
        local Native = GR.Native
        if not Native or not Native.TakeAnchorHealth then return end
        local ply = player.GetAll()[1]
        if not IsValid(ply) then return end
        local max = math.max(ply:GetMaxHealth(), 1)
        local take = Native.TakeAnchorHealth()
        if take >= 0 and ply:Alive() then
            local health = math.max(math.Round(take * max), 1)
            if health ~= ply:Health() then
                ply:SetHealth(health)
                GR.Print(string.format("health: %d, %s", health,
                    take >= 1 and "a new session starts at full health" or "carried over from RDR2's player"))
            end
        end
        Native.SetPlayerHealth(ply:Alive() and math.Clamp(ply:Health() / max, 0, 1) or 0)
    end)

    hook.Add("PlayerDeath", "GR.Health", function(ply)
        if Anchored() then ply.gr_respawn_at = CurTime() + delay:GetFloat() end
    end)

    hook.Add("PlayerDeathThink", "GR.Health", function(ply)
        local at = ply.gr_respawn_at
        if not at or delay:GetFloat() <= 0 or CurTime() < at then return end
        ply.gr_respawn_at = nil
        ply:Spawn()
        return true
    end)

    hook.Add("PlayerSpawn", "GR.Health", function(ply) ply.gr_respawn_at = nil end)

    -- Works around: GMod's death beep is the HEV suit's voice, played at `suitvolume` (0.25)
    -- on top of the game's volume, which in a GMod hidden behind RDR2 is too quiet to hear
    -- (user: "after death the gmod beep should play"). While linked the client plays it at
    -- full volume instead (below), so the quiet one is left out.
    hook.Add("PlayerDeathSound", "GR.Health", function()
        if Anchored() then return true end
    end)
    return
end

-- The HEV suit's death sentence (HL2's HEV_DEAD0): three beeps, then the flatline.
local BEEP, FLATLINE = "hl1/fvox/beep.wav", "hl1/fvox/flatline.wav"

function Health.Beep()
    for i = 0, 2 do
        timer.Simple(i * 0.3, function() surface.PlaySound(BEEP) end)
    end
    timer.Simple(0.9, function() surface.PlaySound(FLATLINE) end)
end

local was_alive = true
hook.Add("Think", "GR.Health", function()
    local ply = LocalPlayer()
    if not IsValid(ply) then return end
    local alive = ply:Alive()
    if was_alive and not alive and Anchored() then Health.Beep() end
    was_alive = alive
end)
