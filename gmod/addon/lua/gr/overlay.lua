-- The overlay: GMod's HUD, menus, viewmodel and physgun beam drawn over RDR2.
--
-- While GMod has RDR2's player, GMod's own world is wiped from its frame just before the
-- viewmodels are drawn, so what is left is exactly what belongs on top of RDR2: the
-- viewmodel (with the physgun beam), the HUD and every VGUI panel, over a transparent
-- clear. The module reads that frame back at Present and shows it in a click-through
-- window over RDR2 (gmod/module/src/overlay_window.h).

local GR = GR
GR.Overlay = GR.Overlay or {}
local Overlay = GR.Overlay

local enabled = CreateClientConVar("gr_overlay", "1", true, false,
    "Garry's Redemption: 1 = show GMod's HUD, menus and viewmodel over RDR2")
-- gr::OverlayAlpha in the module. 0 (raw) is right now that everything writes real alpha;
-- the others are kept for checking: 3 shows GMod's whole frame, world and all.
local alpha_mode = CreateClientConVar("gr_overlay_alpha_rule", "0", false, false,
    "Garry's Redemption: how the overlay's transparency is read. 0 raw, 1 squared, 2 black key, 3 opaque (debug), 4 coverage")

-- D3DKMT scheduling class for GMod's GPU work (module.cpp, GpuPriority): 2 normal, 3 above
-- normal, 4 high. GMod's frame is tiny next to RDR2's, so letting it go first shortens the
-- overlay's delay without RDR2 noticing.
local gpu_priority = CreateClientConVar("gr_overlay_gpu_priority", "4", true, false,
    "Garry's Redemption: GMod's GPU scheduling class while linked: 2 normal, 3 above normal, 4 high")

Overlay.active = false
local EndOverride, KeepAlphaPanel
local applied_priority

-- Once per frame from the bridge, after the module's Tick.
function Overlay.Frame()
    EndOverride()
    local active = enabled:GetBool() and GR.Native.Anchored()
    Overlay.active = active
    if active then KeepAlphaPanel() end
    GR.Native.Overlay(active, active, alpha_mode:GetInt())
    local want = active and gpu_priority:GetInt() or 2
    if want ~= applied_priority and GR.Native.GpuPriority then
        applied_priority = want
        GR.Native.GpuPriority(want)
    end
end

-- GMod's world is replaced by RDR2's, so it is wiped from the frame: no skybox, no opaque
-- entities (the proxies are invisible anyway), and colour and depth cleared to transparent
-- as the translucent pass begins, after the world's brushes (seen: clearing before the
-- opaque entities left the brushes, which come later). The translucent pass is where the
-- physgun draws its beam (seen: clearing before the viewmodels wiped the beam too). Depth
-- is cleared so neither the beam nor the viewmodel is ever cut by GMod geometry RDR2 does
-- not have. Only the depth-pass flag is checked: on gm_flatgrass the main view reports
-- itself as a skybox pass as well (logged: false, false, true every frame).
hook.Add("PreDrawSkyBox", "GR.Overlay", function()
    if Overlay.active and alpha_mode:GetInt() ~= 3 then return true end
end)

hook.Add("PreDrawOpaqueRenderables", "GR.Overlay", function(depth)
    if Overlay.active and alpha_mode:GetInt() ~= 3 and not depth then return true end
end)

hook.Add("PreDrawTranslucentRenderables", "GR.Overlay", function(depth)
    if not Overlay.active or alpha_mode:GetInt() == 3 or depth then return end
    render.Clear(0, 0, 0, 0, true, true)
    -- Source writes depth into the alpha channel by default, which made the viewmodel
    -- translucent in the overlay (seen: alpha falling off along the gun). Plain alpha
    -- makes it opaque.
    render.SetWriteDepthToDestAlpha(false)
    -- Works around: with GMod's own world wiped the frame is nearly black, so its HDR
    -- auto-exposure climbs to the top of its range (logged: tone mapping scale 6.0) and lit
    -- everything six times too bright for RDR2's scene (seen: a crate glowing at night). The
    -- light from light_sync.h is RDR2's, so no exposure of GMod's own goes on top of it.
    render.SetToneMappingScaleLinear(Vector(1, 1, 1))
    -- GMod's props and NPCs, drawn back into the cleared frame behind RDR2's surfaces.
    if GR.WorldRender and GR.WorldRender.Draw then GR.WorldRender.Draw() end
    -- Works around the user's "the pulse rifle energy ball has a black square surrounding it
    -- like a picture": what the engine draws itself from here on (the AR2's ball, sprites,
    -- tracers, beams) is additive, but writes its texture's alpha too, opaque over the whole
    -- square, and the overlay shows GMod's frame by its alpha, so the square's black corners
    -- covered RDR2. With alpha writes off the alpha stays at the clear's 0 and the glow's
    -- colour is added onto RDR2's picture, as GMod draws it. The ball ignores RenderOverride
    -- (seen: never called), so it is done for the rest of the pass.
    render.OverrideAlphaWriteEnable(true, false)
    Overlay.glow_pass = true
end)

local function EndGlowPass()
    if not Overlay.glow_pass then return end
    render.OverrideAlphaWriteEnable(false)
    Overlay.glow_pass = false
end
hook.Add("PostDrawTranslucentRenderables", "GR.Overlay", function(depth)
    if not depth then EndGlowPass() end
end)

hook.Add("PreDrawViewModels", "GR.Overlay", function()
    EndGlowPass()
    if Overlay.active then render.SetWriteDepthToDestAlpha(false) end
end)

-- Works around: a halo (the physgun outlines what it holds with one) copies the whole frame
-- and draws it back, GMod's world included, so the overlay showed GMod's ground and black
-- sky while holding anything (seen). The PreDrawHalos hook cannot stop that, so the halo
-- library's Render is wrapped. A halo would only outline an invisible proxy anyway.
if halo and halo.Render and not Overlay.halo_render then
    Overlay.halo_render = halo.Render
    halo.Render = function(...)
        if Overlay.active then return end
        return Overlay.halo_render(...)
    end
end

-- The viewmodel and hands are lit with RDR2's light instead of gm_flatgrass's daylight
-- (seen: the gun glowing as at noon in RDR2's night): the sun or moon as one directional
-- light, nothing of it in shade or indoors, and the sky as ambient, brighter from above
-- (rdr2/src/light_sync.h works the values out).
local light_enabled = CreateClientConVar("gr_viewmodel_light", "1", true, false,
    "Garry's Redemption: 1 = light the viewmodel with RDR2's sun, shade and time of day")
local LIGHTS = { { type = MATERIAL_LIGHT_DIRECTIONAL, color = Vector(), dir = Vector() } }
local lit = false

-- The light RDR2 sends comes from its clock and knows nothing of weather, fog, lamps or
-- RDR2's exposure, so GMod's things were too bright or too dark for the scene around them
-- (user report: props did not fit in). The module measures RDR2's picture (scene_probe.h)
-- and the light is scaled until a mid-grey thing would come out `gr_light_match` times as
-- bright as the scene, and the sky's light takes half of the scene's tint.
local match = CreateClientConVar("gr_light_match", "2.2", true, false,
    "Garry's Redemption: how bright GMod's things are lit against RDR2's picture (0 = RDR2's clock only)")
local gain, gain_frame = 1, -1
local tint = Vector(1, 1, 1)

local function Lum(r, g, b) return 0.2126 * r + 0.7152 * g + 0.0722 * b end

timer.Create("GR.Overlay.SceneProbe", 1, 0, function()
    if GR.Native and GR.Native.SceneProbe then
        GR.Native.SceneProbe(Overlay.active == true and light_enabled:GetBool() and match:GetFloat() > 0)
    end
end)

-- Once a frame: how much to scale RDR2's light by, and the scene's tint.
local function Match(r, g, b, ar, ag, ab)
    if gain_frame == FrameNumber() then return end
    gain_frame = FrameNumber()
    local want, wr, wg, wb = 1, 1, 1, 1
    local sr, sg, sb
    if match:GetFloat() > 0 and GR.Native.SceneColor then sr, sg, sb = GR.Native.SceneColor() end
    if sr then
        local scene = Lum(sr, sg, sb)
        local model = Lum(ar, ag, ab) + 0.5 * Lum(r, g, b)
        want = math.Clamp(match:GetFloat() * scene / math.max(model, 1e-4), 0.15, 6)
        if scene > 1e-4 then
            wr, wg, wb = math.Clamp(sr / scene, 0.5, 1.6), math.Clamp(sg / scene, 0.5, 1.6), math.Clamp(sb / scene, 0.5, 1.6)
        end
    end
    local k = math.min(RealFrameTime() * 2, 1)
    gain = gain + (want - gain) * k
    tint.x = tint.x + (wr - tint.x) * k
    tint.y = tint.y + (wg - tint.y) * k
    tint.z = tint.z + (wb - tint.z) * k
end

-- For gr_overlay_status and world_render.lua's shadows.
function Overlay.LightGain() return gain end

local function BeginLight()
    if not Overlay.active or not light_enabled:GetBool() then return end
    local dx, dy, dz, r, g, b, ar, ag, ab = GR.Native.Light()
    if not dx then return end
    Match(r, g, b, ar, ag, ab)
    r, g, b = r * gain, g * gain, b * gain
    local al = Lum(ar, ag, ab) * gain
    ar, ag, ab = (ar * gain + al * tint.x) / 2, (ag * gain + al * tint.y) / 2, (ab * gain + al * tint.z) / 2
    local light = LIGHTS[1]
    light.dir:SetUnpacked(-dx, -dy, -dz)  -- the way the light travels
    light.dir:Normalize()
    light.color:SetUnpacked(r, g, b)
    render.SuppressEngineLighting(true)
    render.ResetModelLighting(ar, ag, ab)
    render.SetModelLighting(BOX_TOP, ar * 1.4, ag * 1.4, ab * 1.4)
    render.SetModelLighting(BOX_BOTTOM, ar * 0.45, ag * 0.45, ab * 0.45)
    render.SetLocalModelLights(LIGHTS)
    lit = true
end

local function EndLight()
    if not lit then return end
    render.SuppressEngineLighting(false)
    render.SetLocalModelLights()
    lit = false
end

-- world_render.lua lights GMod's props the same way.
Overlay.BeginLight = BeginLight
Overlay.EndLight = EndLight

hook.Add("PreDrawViewModel", "GR.Overlay.Light", function() BeginLight() end)
hook.Add("PostDrawViewModel", "GR.Overlay.Light", function() EndLight() end)
hook.Add("PreDrawPlayerHands", "GR.Overlay.Light", function() BeginLight() end)
hook.Add("PostDrawPlayerHands", "GR.Overlay.Light", function() EndLight() end)

-- The menu cursor (input.lua moves it): an arrow with a dark outline, drawn last so it is
-- over every panel.
-- Two convex pieces (surface.DrawPoly wants convex, clockwise): the head and the tail.
local ARROW = {
    { { 0, 0 }, { 12, 12 }, { 0, 17 } },
    { { 4, 13 }, { 7, 12 }, { 10, 19 }, { 7, 20 } },
}
local function Piece(points, x, y, scale, grow)
    local cx, cy = 0, 0
    for _, p in ipairs(points) do cx, cy = cx + p[1], cy + p[2] end
    cx, cy = cx / #points, cy / #points
    local poly = {}
    for i, p in ipairs(points) do
        -- grow pushes each corner away from the piece's middle, for the outline.
        local ox, oy = p[1] - cx, p[2] - cy
        local len = math.max(math.sqrt(ox * ox + oy * oy), 0.001)
        poly[i] = { x = x + p[1] * scale + ox / len * grow, y = y + p[2] * scale + oy / len * grow }
    end
    return poly
end

hook.Add("DrawOverlay", "GR.Overlay.Cursor", function()
    local c = GR.Input and GR.Input.cursor
    if not Overlay.active or not c or not vgui.CursorVisible() then return end
    local scale = math.max(ScrH() / 1080, 1) * 1.2
    draw.NoTexture()
    surface.SetDrawColor(0, 0, 0, 255)
    for _, piece in ipairs(ARROW) do surface.DrawPoly(Piece(piece, c.x, c.y, scale, 1.5 * scale)) end
    surface.SetDrawColor(255, 255, 255, 255)
    for _, piece in ipairs(ARROW) do surface.DrawPoly(Piece(piece, c.x, c.y, scale, 0)) end
end)

-- Works around: the HUD and VGUI write colour but leave the alpha channel alone, so the
-- overlay could only guess coverage from brightness, and dark opaque pixels (icon shadows,
-- text) came out see-through. While they draw, alpha is written and blended as "over"
-- (source 1, destination 1 - source alpha), colour as usual, so the frame ends up as
-- premultiplied colour with true alpha, which the overlay shows as it is.
--
-- The HUD draws inside GMod's view, where PreDrawHUD can turn this on. VGUI paints in a
-- pass of its own that starts with the render state reset (seen: the override reached the
-- HUD but not the spawn menu), so an invisible full-screen panel kept behind every other
-- panel turns it on again from inside that pass. It lasts until the next frame begins.
local function BeginOverride()
    render.OverrideAlphaWriteEnable(true, true)
    render.OverrideBlend(true, BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA, BLENDFUNC_ADD,
        BLEND_ONE, BLEND_ONE_MINUS_SRC_ALPHA, BLENDFUNC_ADD)
    Overlay.overriding = true
end

function EndOverride()
    if not Overlay.overriding then return end
    render.OverrideBlend(false)
    render.OverrideAlphaWriteEnable(false)
    Overlay.overriding = false
end

hook.Add("PreDrawHUD", "GR.Overlay", function()
    if Overlay.active then BeginOverride() end
end)

function KeepAlphaPanel()
    local panel = Overlay.alpha_panel
    if not IsValid(panel) then
        panel = vgui.Create("Panel")
        panel:SetMouseInputEnabled(false)
        panel:SetKeyboardInputEnabled(false)
        panel.Paint = function()
            if Overlay.active then BeginOverride() end
        end
        Overlay.alpha_panel = panel
    end
    if panel:GetWide() ~= ScrW() or panel:GetTall() ~= ScrH() then panel:SetSize(ScrW(), ScrH()) end
    panel:MoveToBack()
end

concommand.Add("gr_overlay_status", function()
    local s = GR.Native and GR.Native.OverlayStats and GR.Native.OverlayStats()
    if not s then
        GR.Print("overlay: the module is not loaded.")
        return
    end
    GR.Print(string.format("overlay: hook %s, capture %s, %d presents, %d captures, %d failed, last %.2f ms",
        s.hooked and "in" or "MISSING", s.capturing and "on" or "off", s.presents, s.captures, s.failures,
        s.capture_us / 1000))
    GR.Print(string.format("  back buffer %dx%d, format %d, multisample %d", s.width, s.height, s.format,
        s.multisample))
    GR.Print(string.format("  window %s, %s at (%d, %d) %dx%d, %d frames shown, upload %.2f ms, capture to screen %.2f ms (average %.2f), alpha mode %d",
        s.window and "ready" or "NOT READY", s.shown and "shown" or "hidden", s.x, s.y, s.w, s.h, s.frames,
        s.upload_us / 1000, s.age_us / 1000, (s.avg_age_us or 0) / 1000, s.alpha))
end, nil, "Garry's Redemption: show what the overlay is doing")

concommand.Add("gr_overlay_dump", function(_, _, _, path)
    -- The whole argument string: the console splits "C:/..." at the colon.
    if not GR.Native or not path or path == "" then return end
    GR.Native.OverlayDump(path)
end, nil, "Garry's Redemption: write the next captured frame to the given file (raw BGRA)")
