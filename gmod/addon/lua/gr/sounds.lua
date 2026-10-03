-- GMod's sound while it is hidden behind RDR2.
--
-- GMod's own gunfire is what the player hears. An earlier version muted it and left the
-- sound to RDR2's bullet (combat_sync.h, Shot), but a bullet shot between two points has no
-- gun behind it and makes no gunshot sound: the user heard no shots at all. The bullet stays,
-- for RDR2's people to react to. `gr_mute_gunshots 1` brings the muting back.

local GR = GR

local mute = CreateConVar("gr_mute_gunshots", "0", FCVAR_ARCHIVE + FCVAR_REPLICATED,
    "Garry's Redemption: 1 = GMod's gunfire sounds are muted while linked to RDR2")

-- Sound script names of firing sounds: HL2's "Weapon_Pistol.Single", "Weapon_Shotgun.Double",
-- "Weapon_SMG1.Burst" and the like.
local function IsGunshot(name)
    return name ~= nil and name:find("^Weapon_") ~= nil
        and (name:find("%.Single$") or name:find("%.Double$") or name:find("%.Burst$")) ~= nil
end

hook.Add("EntityEmitSound", "GR.Sounds", function(data)
    if not mute:GetBool() or not GR.Native or not GR.Native.Anchored or not GR.Native.Anchored() then return end
    if IsGunshot(data.OriginalSoundName) then return false end
end)

if SERVER then return end

-- GMod's audio comes out of a window that is never really in front. Works around two things
-- the user heard: sound cutting off (snd_mute_losefocus mutes GMod whenever Windows says it
-- lost focus, and the keep-active trick only covers WM_ACTIVATE) and sound lagging
-- (snd_mixahead buffers 100 ms). While linked they are set as below; the user's own values
-- come back when the link goes. Lua may not set them, so the module does (EngineSetting).
-- fps_max: not sound, but set the same way. GMod draws next to nothing, and every
-- millisecond of its frame is a millisecond RDR2's camera has to be held back for the view
-- lock (player.lua). The user's cap (150 here) comes back when the link goes.
local LINKED = { snd_mute_losefocus = "0", snd_mixahead = "0.05", fps_max = "300" }
local saved

timer.Create("GR.Sounds", 1, 0, function()
    local linked = GR.Native and GR.Native.State and GR.Native.State() == "connected"
    if linked and not saved then
        saved = {}
        for name, value in pairs(LINKED) do
            saved[name] = GetConVar(name) and GetConVar(name):GetString()
            if GR.Native.EngineSetting then GR.Native.EngineSetting(name, tonumber(value)) end
        end
    elseif not linked and saved then
        for name, value in pairs(saved) do
            if value and GR.Native and GR.Native.EngineSetting then GR.Native.EngineSetting(name, tonumber(value)) end
        end
        saved = nil
    end
end)
