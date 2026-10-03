-- Input: plays the keyboard and mouse RDR2 captured onto the GMod player.
--
-- GMod's window is not in front, so the engine reads no keys and no mouse of its own. The
-- module hands over RDR2's key changes (Windows virtual-key codes) and mouse counts. Each
-- key is looked up in the user's own GMod bindings, so their binds work as they are:
--   movement and button binds (+forward, +jump, +attack ...) are held in a table and put
--     into every user command in CreateMove,
--   any other bind (noclip, +menu, impulse 100, slot1 ...) is run as the console command
--     it is, with the matching "-" command on release.
-- The view angles are kept here and turned by the mouse counts with GMod's own
-- sensitivity, m_yaw and m_pitch.

local GR = GR
GR.Input = GR.Input or {}
local Input = GR.Input

-- Windows virtual-key code -> GMod button code. Left and right Shift, Ctrl and Alt arrive
-- under their own codes (0xA0 to 0xA5); the generic 0x10 to 0x12 are left out so one
-- press is not two.
local VK = {
    [0x01] = MOUSE_LEFT, [0x02] = MOUSE_RIGHT, [0x04] = MOUSE_MIDDLE, [0x05] = MOUSE_4, [0x06] = MOUSE_5,
    [0x08] = KEY_BACKSPACE, [0x09] = KEY_TAB, [0x0D] = KEY_ENTER, [0x14] = KEY_CAPSLOCK, [0x20] = KEY_SPACE,
    [0x21] = KEY_PAGEUP, [0x22] = KEY_PAGEDOWN, [0x23] = KEY_END, [0x24] = KEY_HOME,
    [0x25] = KEY_LEFT, [0x26] = KEY_UP, [0x27] = KEY_RIGHT, [0x28] = KEY_DOWN,
    [0x2D] = KEY_INSERT, [0x2E] = KEY_DELETE,
    [0xA0] = KEY_LSHIFT, [0xA1] = KEY_RSHIFT, [0xA2] = KEY_LCONTROL, [0xA3] = KEY_RCONTROL,
    [0xA4] = KEY_LALT, [0xA5] = KEY_RALT,
    [0xBA] = KEY_SEMICOLON, [0xBB] = KEY_EQUAL, [0xBC] = KEY_COMMA, [0xBD] = KEY_MINUS,
    [0xBE] = KEY_PERIOD, [0xBF] = KEY_SLASH, [0xC0] = KEY_BACKQUOTE,
    [0xDB] = KEY_LBRACKET, [0xDC] = KEY_BACKSLASH, [0xDD] = KEY_RBRACKET, [0xDE] = KEY_APOSTROPHE,
}
for i = 0, 9 do VK[0x30 + i] = KEY_0 + i end       -- 0 to 9
for i = 0, 25 do VK[0x41 + i] = KEY_A + i end      -- A to Z
for i = 0, 9 do VK[0x60 + i] = KEY_PAD_0 + i end   -- numeric keypad
for i = 0, 11 do VK[0x70 + i] = KEY_F1 + i end     -- F1 to F12

-- Binds that go into the user command rather than the console.
local MOVE = {
    ["+forward"] = { "forward", 1 }, ["+back"] = { "forward", -1 },
    ["+moveright"] = { "side", 1 }, ["+moveleft"] = { "side", -1 },
    ["+moveup"] = { "up", 1 }, ["+movedown"] = { "up", -1 },
}
local BUTTON = {
    ["+attack"] = IN_ATTACK, ["+attack2"] = IN_ATTACK2, ["+jump"] = IN_JUMP, ["+duck"] = IN_DUCK,
    ["+use"] = IN_USE, ["+reload"] = IN_RELOAD, ["+speed"] = IN_SPEED, ["+walk"] = IN_WALK,
    ["+zoom"] = IN_ZOOM, ["+alt1"] = IN_ALT1, ["+alt2"] = IN_ALT2,
}
-- Binds that must not fire from here: they act on the hidden GMod window itself, and
-- "pause" would stop the game the RDR2 player is standing in.
local NEVER = {
    ["toggleconsole"] = true, ["cancelselect"] = true, ["pause"] = true, ["quit"] = true, ["exit"] = true,
    ["disconnect"] = true, ["jpeg"] = true, ["screenshot"] = true, ["save"] = true, ["load"] = true,
}

local enabled = CreateClientConVar("gr_input", "1", false, false,
    "Garry's Redemption: 1 = RDR2's keyboard and mouse drive the GMod player")

Input.active = false      -- true while this file, not the engine, decides the player's input
Input.view = Input.view or Angle(0, 0, 0)
local move = { forward = 0, side = 0, up = 0 }
local buttons = 0
local held = {}           -- vk -> what to undo when the key comes up

local sensitivity = GetConVar("sensitivity")
local m_yaw = GetConVar("m_yaw")
local m_pitch = GetConVar("m_pitch")
local forward_speed = GetConVar("cl_forwardspeed")
local side_speed = GetConVar("cl_sidespeed")
local up_speed = GetConVar("cl_upspeed")

-- Mouse buttons, as GMod's MOUSE_ codes, for the menus.
local MENU_MOUSE = { [0x01] = MOUSE_LEFT, [0x02] = MOUSE_RIGHT, [0x04] = MOUSE_MIDDLE }

-- What a key types, unshifted and shifted (US layout), for text boxes in the menus.
local TYPED = { [0x20] = { " ", " " }, [0xBA] = { ";", ":" }, [0xBB] = { "=", "+" }, [0xBC] = { ",", "<" },
    [0xBD] = { "-", "_" }, [0xBE] = { ".", ">" }, [0xBF] = { "/", "?" }, [0xC0] = { "`", "~" },
    [0xDB] = { "[", "{" }, [0xDC] = { "\\", "|" }, [0xDD] = { "]", "}" }, [0xDE] = { "'", "\"" } }
local SHIFTED_DIGITS = ")!@#$%^&*("
for i = 0, 9 do TYPED[0x30 + i] = { tostring(i), string.sub(SHIFTED_DIGITS, i + 1, i + 1) } end
for i = 0, 25 do TYPED[0x41 + i] = { string.char(97 + i), string.char(65 + i) } end

local shift_down = 0

-- Works around: Backspace did nothing in the spawn menu's search (user report). A text box
-- edits on the "key code typed" event (Backspace, Delete, the arrows, Home, End, Enter), which
-- the engine sends with every press and every repeat of a held key; only "pressed" was sent,
-- so only the letters, which come as typed characters, worked. Both are sent now, and a held
-- key repeats as Windows' would.
local REPEAT_DELAY, REPEAT_EVERY = 0.5, 0.033
local MODIFIER = { [0xA0] = true, [0xA1] = true, [0xA2] = true, [0xA3] = true, [0xA4] = true, [0xA5] = true,
    [0x14] = true }

local function TypeKey(vk, key)
    gui.InternalKeyCodeTyped(key)
    local typed = TYPED[vk]
    if typed then gui.InternalKeyTyped(string.byte(typed[shift_down > 0 and 2 or 1])) end
end

-- While a GMod menu has the cursor, the mouse buttons click its panels, and keys go to the
-- text box that has the keyboard (the spawn menu's search, chat), as GMod does. Keys go to
-- their binds otherwise, so letting go of Q still closes the spawn menu.
local function PressMenu(vk, key)
    local button = MENU_MOUSE[vk]
    if button then
        gui.InternalMousePressed(button)
        held[vk] = { gui_mouse = button }
        return true
    end
    if not IsValid(vgui.GetKeyboardFocus()) then return false end
    gui.InternalKeyCodePressed(key)
    TypeKey(vk, key)
    held[vk] = { gui_key = key, repeat_at = not MODIFIER[vk] and SysTime() + REPEAT_DELAY or nil }
    return true
end

-- GMod's own camera commands become Garry's Redemption's (thirdperson.lua): GMod's need
-- cheats on and would move only GMod's camera, not RDR2's. Whatever key the user gave them
-- (here L and B) switches the view in both games; thirdperson switches back and forth.
local REDIRECT = { thirdperson = "gr_thirdperson", firstperson = "gr_firstperson" }

local function Press(vk)
    if vk == 0xA0 or vk == 0xA1 then shift_down = shift_down + 1 end
    local key = VK[vk]
    if not key then return end
    if vgui.CursorVisible() and PressMenu(vk, key) then return end
    local bind = input.LookupKeyBinding(key)
    if not bind or bind == "" then
        -- A key with no bind still means something to GMod: the numpad keys that fire
        -- thrusters and other tool-made things reach the server as the engine's own key
        -- presses (PlayerButtonDown), so the engine is handed the real key.
        if GR.Native.PostButton(vk, true) then held[vk] = { posted = true } end
        return
    end
    bind = string.lower(bind)
    if NEVER[bind] then return end
    bind = REDIRECT[bind] or bind

    local axis = MOVE[bind]
    if axis then
        move[axis[1]] = move[axis[1]] + axis[2]
        held[vk] = { axis = axis }
    elseif BUTTON[bind] then
        buttons = bit.bor(buttons, BUTTON[bind])
        held[vk] = { button = BUTTON[bind] }
        -- Works around: the weapon selection HUD takes its confirming click from the
        -- engine's key events, which this hidden window never gets, so a highlighted
        -- weapon was never picked (user report). The engine is handed the real key too,
        -- and CreateMove fires only if the engine did not keep the press for the HUD.
        if bind == "+attack" and GR.Native.PostButton(vk, true) then held[vk].posted = true end
    else
        LocalPlayer():ConCommand(bind)
        -- A "+command" stays on until its "-command" is run.
        held[vk] = { release = string.sub(bind, 1, 1) == "+" and ("-" .. string.sub(bind, 2)) or nil }
    end
end

local function Release(vk)
    if vk == 0xA0 or vk == 0xA1 then shift_down = math.max(shift_down - 1, 0) end
    local undo = held[vk]
    if not undo then return end
    held[vk] = nil
    if undo.posted then GR.Native.PostButton(vk, false) end
    if undo.gui_mouse then
        gui.InternalMouseReleased(undo.gui_mouse)
    elseif undo.gui_key then
        gui.InternalKeyCodeReleased(undo.gui_key)
    elseif undo.axis then
        move[undo.axis[1]] = move[undo.axis[1]] - undo.axis[2]
    elseif undo.button then
        -- Two keys can be bound to the same button: keep it down while either is.
        local still = false
        for _, other in pairs(held) do
            if other.button == undo.button then still = true end
        end
        if not still then buttons = bit.band(buttons, bit.bnot(undo.button)) end
    elseif undo.release then
        LocalPlayer():ConCommand(undo.release)
    end
end

local wheel_cmd = 0  -- notches not yet put into a user command

-- The wheel has no key state: each notch is one press of whatever is bound to it. It also
-- goes into the user command, which is where the physgun reads it from to push and pull
-- what it holds; while it holds something the weapon-switch binds stay quiet, as they do
-- in GMod.
local function Wheel(notches)
    wheel_cmd = wheel_cmd + notches
    local weapon = LocalPlayer():GetActiveWeapon()
    if bit.band(buttons, IN_ATTACK) ~= 0 and IsValid(weapon) and weapon:GetClass() == "weapon_physgun" then return end
    local key = notches > 0 and MOUSE_WHEEL_UP or MOUSE_WHEEL_DOWN
    local bind = input.LookupKeyBinding(key)
    if not bind or bind == "" or NEVER[string.lower(bind)] then return end
    for _ = 1, math.min(math.abs(notches), 8) do
        LocalPlayer():ConCommand(bind)
    end
end

-- gui.HideGameUI is rate limited by the engine: try once a second, not every frame. Called
-- from Frame (PreRender) because Think stops while the menu has the game paused.
local next_hide = 0
local function KeepGameUIClosed()
    if RealTime() < next_hide or not gui.IsGameUIVisible() then return end
    next_hide = RealTime() + 1
    gui.HideGameUI()
end

local predict = CreateClientConVar("gr_look_predict", "1", true, false,
    "Garry's Redemption: 1 = RDR2 turns its camera with the mouse at once, not a frame or two later (a smoother look)")

local cursor_speed = CreateClientConVar("gr_cursor_speed", "1", true, false,
    "Garry's Redemption: menu cursor pixels per mouse count")

-- GMod's window never sees the mouse, so the menu cursor is kept here and handed to VGUI
-- every frame (gui.InternalCursorMoved takes a screen position, whatever the wiki says:
-- seen moving the hovered panel). It starts in the middle each time a menu opens, and
-- overlay.lua draws it, since the system cursor is not in the captured frame.
local function MoveCursor(dx, dy, wheel)
    local c = Input.cursor
    if not c then
        c = { x = ScrW() / 2, y = ScrH() / 2 }
        Input.cursor = c
    end
    local speed = cursor_speed:GetFloat()
    c.x = math.Clamp(c.x + dx * speed, 0, ScrW() - 1)
    c.y = math.Clamp(c.y + dy * speed, 0, ScrH() - 1)
    gui.InternalCursorMoved(math.floor(c.x), math.floor(c.y))
    if wheel ~= 0 then
        Input.wheel_rest = (Input.wheel_rest or 0) + wheel
        local notches = math.modf(Input.wheel_rest / 120)
        if notches ~= 0 then
            Input.wheel_rest = Input.wheel_rest - notches * 120
            gui.InternalMouseWheeled(notches)
        end
    end
end

-- Lua asks the system for the cursor (tooltips place themselves with gui.MousePos, and
-- landed in the top-left corner, seen), which is not where the menu cursor is. While it
-- is ours, these answer with it.
if not Input.real_cursor then
    Input.real_cursor = { input.GetCursorPos, gui.MousePos, gui.MouseX, gui.MouseY }
    local real = Input.real_cursor
    input.GetCursorPos = function()
        local c = Input.cursor
        if c then return math.floor(c.x), math.floor(c.y) end
        return real[1]()
    end
    gui.MousePos = function()
        local c = Input.cursor
        if c then return math.floor(c.x), math.floor(c.y) end
        return real[2]()
    end
    gui.MouseX = function()
        local c = Input.cursor
        if c then return math.floor(c.x) end
        return real[3]()
    end
    gui.MouseY = function()
        local c = Input.cursor
        if c then return math.floor(c.y) end
        return real[4]()
    end
end

-- Points the view. Used when the player is anchored to where RDR2's ped stands.
function Input.SetView(pitch, yaw)
    Input.view = Angle(math.Clamp(math.NormalizeAngle(pitch), -89, 89), math.NormalizeAngle(yaw), 0)
    LocalPlayer():SetEyeAngles(Input.view)
end

-- Once per frame, after the module has read RDR2's frame.
function Input.Frame()
    -- For as long as GMod has RDR2's player, and not only while RDR2's window is the one
    -- in front. Works around: GMod kept active behind other windows goes on turning its
    -- own view from wherever the cursor is (seen in-game: the view drifted until it faced
    -- straight down whenever another window was in front of RDR2). While the input is
    -- not captured the module reports no keys and no mouse, so the player just stands.
    local active = GR.Native.Anchored() and enabled:GetBool()

    -- Always drain the key changes: when capture ends the module reports every held key
    -- as released, which is what lets go of everything here.
    while true do
        local vk, down = GR.Native.NextKey()
        if not vk then break end
        if down and active then Press(vk) else Release(vk) end
    end
    local now = SysTime()
    for vk, h in pairs(held) do
        if h.repeat_at and now >= h.repeat_at then
            if IsValid(vgui.GetKeyboardFocus()) then TypeKey(vk, h.gui_key) end
            h.repeat_at = now + REPEAT_EVERY
        end
    end
    local dx, dy, wheel = GR.Native.Mouse()

    if not active then
        if GR.Native.SetMouseLook then GR.Native.SetMouseLook(0, 0) end
        -- GMod on its own looks after its own view: follow it, so nothing jumps later.
        -- Not while RDR2 is connected and only has the player for now (F9, a scene): the
        -- kept-active GMod's view drifts then (see Frame), and would be taken up on return.
        Input.active = false
        if GR.Native.State() ~= "connected" then Input.view = LocalPlayer():EyeAngles() end
        return
    end
    Input.active = true
    KeepGameUIClosed()

    -- A GMod menu with a cursor is open: the mouse is for the menu, not the view.
    if vgui.CursorVisible() then
        if GR.Native.SetMouseLook then GR.Native.SetMouseLook(0, 0) end
        MoveCursor(dx, dy, wheel)
    else
        -- RDR2 turns its camera by the counts that have not reached here yet (player_sync.h).
        if GR.Native.SetMouseLook then
            local s = predict:GetBool() and sensitivity:GetFloat() or 0
            GR.Native.SetMouseLook(-m_yaw:GetFloat() * s, m_pitch:GetFloat() * s)
        end
        Input.cursor = nil
        local view = Input.view
        local scale = sensitivity:GetFloat()
        view.yaw = math.NormalizeAngle(view.yaw - dx * m_yaw:GetFloat() * scale)
        view.pitch = math.Clamp(view.pitch + dy * m_pitch:GetFloat() * scale, -89, 89)
        view.roll = 0
        if wheel ~= 0 then
            Input.wheel_rest = (Input.wheel_rest or 0) + wheel
            local notches = math.modf(Input.wheel_rest / 120)
            if notches ~= 0 then
                Input.wheel_rest = Input.wheel_rest - notches * 120
                Wheel(notches)
            end
        end
    end
    -- The engine's own view angles: what the next user command and the renderer start from.
    LocalPlayer():SetEyeAngles(Input.view)
end

-- Works around: GMod's game menu pauses single player, so with it open the RDR2 player
-- cannot move, and the window it is in is hidden, so nobody can see it to close it (seen
-- in-game: it was left open after Escape reached the hidden window). While GMod has
-- RDR2's player the menu is refused, and closed if it got open anyway. Shift+Escape in
-- GMod's own window still opens it (the hook is skipped then).
hook.Add("OnPauseMenuShow", "GR.Input", function()
    if Input.active then return false end
end)


-- Works around: a GMod kept active while hidden still feeds its own idea of the mouse into
-- the command. While GMod has RDR2's player the command is rebuilt from nothing but what
-- came over the bridge.
hook.Add("CreateMove", "GR.Input", function(cmd)
    if not Input.active then return end
    -- The engine's own +attack is down only if the weapon selection HUD did not take the
    -- click (see Press); everything else of the engine's command is thrown away.
    local engine_attack = bit.band(cmd:GetButtons(), IN_ATTACK) ~= 0
    cmd:ClearMovement()
    cmd:SetButtons(engine_attack and buttons or bit.band(buttons, bit.bnot(IN_ATTACK)))
    cmd:SetForwardMove(move.forward * forward_speed:GetFloat())
    cmd:SetSideMove(move.side * side_speed:GetFloat())
    cmd:SetUpMove(move.up * up_speed:GetFloat())
    cmd:SetViewAngles(Input.view)
    cmd:SetMouseWheel(wheel_cmd)
    wheel_cmd = 0
end)
