-- The health and ammo brackets beside the crosshair, client side, while GMod's frame is the
-- overlay over RDR2.
--
-- Works around: the engine's own brackets (CHUDQuickInfo) leave a 40 by 40 pixel square of
-- alpha with no colour beside the crosshair: the empty part of the ammo bracket's quad. Over
-- GMod's world that shows nothing, over RDR2 it is a gray block (user report). The engine's
-- are hidden while the overlay is on and the same brackets are drawn here as text, which
-- writes alpha only where the glyph is.

local GR = GR
GR.QuickInfo = GR.QuickInfo or {}

-- HL2's bracket font at the engine's size (28 tall on a 480-line screen): [ ] are the full
-- brackets, { } the empty ones. The scheme's own "QuickInfo" font came out half the size in Lua.
local FONT = "GR.QuickInfo"
local font_h
local function Font()
    if font_h ~= ScrH() then
        font_h = ScrH()
        surface.CreateFont(FONT, { font = "HL2cross", size = math.Round(28 * ScrH() / 480), antialias = true })
    end
    surface.SetFont(FONT)
end
local NORMAL = Color(255, 220, 0, 60)
local LOW = Color(255, 48, 0, 150)
local quickinfo = GetConVar("hud_quickinfo")

local function On()
    return GR.Overlay and GR.Overlay.active
end

hook.Add("HUDShouldDraw", "GR.QuickInfo", function(name)
    if name == "CHUDQuickInfo" and On() then return false end
end)

-- One bracket: the empty glyph down to `empty` of its height, the full one below.
local function Bracket(x, y, w, h, full, hollow, empty, color)
    local split = y + math.floor(h * math.Clamp(empty, 0, 1))
    surface.SetTextColor(color)
    if split > y then
        render.SetScissorRect(x, y, x + w, split, true)
        surface.SetTextPos(x, y)
        surface.DrawText(hollow)
    end
    if split < y + h then
        render.SetScissorRect(x, split, x + w, y + h, true)
        surface.SetTextPos(x, y)
        surface.DrawText(full)
    end
    render.SetScissorRect(0, 0, 0, 0, false)
end

hook.Add("HUDPaint", "GR.QuickInfo", function()
    if not On() or (quickinfo and not quickinfo:GetBool()) then return end
    local ply = LocalPlayer()
    if not IsValid(ply) or not ply:Alive() or ply:InVehicle() then return end
    if GR.Third and GR.Third.Active and GR.Third.Active() then return end

    Font()
    local w, h = surface.GetTextSize("[")
    -- Where the engine puts them: one bracket's width clear of the centre on each side.
    local cx, cy = math.floor(ScrW() / 2), math.floor(ScrH() / 2)
    local y = cy - math.floor(h / 2)

    local health = math.Clamp(ply:Health() / math.max(ply:GetMaxHealth(), 1), 0, 1)
    Bracket(cx - w * 2, y, w, h, "[", "{", 1 - health, health <= 0.25 and LOW or NORMAL)

    local weapon = ply:GetActiveWeapon()
    if not IsValid(weapon) then return end
    local max = weapon:GetMaxClip1()
    local ammo
    if max > 0 then
        ammo = weapon:Clip1() / max
    elseif weapon:GetPrimaryAmmoType() >= 0 then
        ammo = ply:GetAmmoCount(weapon:GetPrimaryAmmoType()) > 0 and 1 or 0
    else
        ammo = 1  -- the physgun, the toolgun, the crowbar: nothing to run out of
    end
    Bracket(cx + w, y, w, h, "]", "}", 1 - ammo, (max > 0 and ammo <= 0.25) and LOW or NORMAL)
end)
