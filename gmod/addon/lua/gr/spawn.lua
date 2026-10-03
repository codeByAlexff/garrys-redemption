-- Spawning RDR2 entities (milestone 7): the Q menu's Red Dead tab, undo, cleanup and the
-- remover.
--
-- The tab lists RDR2's people, horses, animals, vehicles and objects by model name
-- (gr/models_data.lua, made by tools/gen_models.py), with search; any other model name can
-- be typed in. A click asks the server, which sends RDR2 a spawn event (model hash, the
-- spot under the crosshair, facing the player) and makes an undo entry. RDR2 makes the
-- entity (rdr2/src/spawn_sync.h), and it comes back as a proxy on its next scan, flagged as
-- spawned. Undo (Z), cleanup and the remover delete the real entity in RDR2.
--
-- GMod's own Source props, NPCs and vehicles cannot be seen over RDR2 yet (milestone 9), so
-- their tabs are hidden; Weapons stays.

local GR = GR
GR.Spawn = GR.Spawn or {}
local Spawn = GR.Spawn

local ENTF_SPAWNED = 16
Spawn.ENTF_SPAWNED = ENTF_SPAWNED

cleanup.Register("gr_rdr2")

if SERVER then
    util.AddNetworkString("gr_spawn")
    util.AddNetworkString("gr_spawn_note")

    Spawn.next_id = Spawn.next_id or 1
    Spawn.pending = Spawn.pending or {}      -- id -> { ply, name }
    Spawn.owner = Spawn.owner or {}          -- RDR2 handle -> the player who spawned it
    Spawn.results_seen = Spawn.results_seen or nil

    local STATUS = {
        [2] = "RDR2 has no model called %s",
        [3] = "%s took too long to load",
        [4] = "Too many RDR2 entities spawned: clean some up first",
        [5] = "RDR2 could not make %s",
    }

    function Spawn.Note(ply, text, kind)
        if not IsValid(ply) then return end
        net.Start("gr_spawn_note")
        net.WriteString(text)
        net.WriteUInt(kind or NOTIFY_GENERIC, 4)
        net.Send(ply)
    end
    local Note = Spawn.Note

    local function Ready()
        return GR.Native and GR.Native.Spawn and GR.Native.Anchored()
    end

    function Spawn.Request(ply, name)
        name = string.lower(string.Trim(name or ""))
        if not string.match(name, "^[%w_]+$") or #name > 64 then return end
        if not Ready() then
            Note(ply, "RDR2 does not have the player: nothing can be spawned", NOTIFY_ERROR)
            return
        end
        local eye, aim = ply:EyePos(), ply:GetAimVector()
        -- Through people and animals: their proxies stand in the way (a spawned person right
        -- in front put the next wagon on the player). Vehicles and objects can be built on.
        local tr = util.TraceLine({ start = eye, endpos = eye + aim * 4000, mask = MASK_SOLID, filter = function(ent)
            return ent ~= ply and not (ent:GetClass() == "gr_proxy" and (ent.gr_kind or 0) <= 3)
        end })
        local pos
        if tr.Hit and not tr.HitSky then
            -- Off a wall, so whatever it is does not start inside it.
            pos = tr.HitPos + (tr.HitNormal.z < 0.7 and tr.HitNormal * 40 or vector_origin)
        else
            pos = eye + aim * 200
        end
        local facing = ply:GetPos() - pos
        facing.z = 0
        if facing:LengthSqr() < 1 then facing = -aim end
        Spawn.At(ply, GR.Native.Joaat(name), name, pos, facing)
    end

    -- Spawns model `hash` at `pos`, its front along `facing`, with an undo entry. `label`
    -- names it in notices and the undo list. Also used by the duplicator (tools.lua).
    function Spawn.At(ply, hash, label, pos, facing)
        if not Ready() then return false end
        local id = Spawn.next_id
        Spawn.next_id = id + 1
        if not GR.Native.Spawn(id, hash, pos.x, pos.y, pos.z, facing.x, facing.y, facing.z) then
            Note(ply, "Could not reach RDR2", NOTIFY_ERROR)
            return false
        end
        Spawn.pending[id] = { ply = ply, name = label }

        undo.Create("RDR2 entity")
        undo.SetCustomUndoText("Undone " .. label)
        undo.AddFunction(function() GR.Native.Remove(0, id, false) end)
        undo.SetPlayer(ply)
        undo.Finish()
        return true
    end

    net.Receive("gr_spawn", function(_, ply)
        if IsValid(ply) and ply:IsListenServerHost() then Spawn.Request(ply, net.ReadString()) end
    end)

    -- RDR2's answers to spawn requests.
    hook.Add("Tick", "GR.Spawn", function()
        local Native = GR.Native
        if not Native or not Native.SpawnResultCount then return end
        local newest = Native.SpawnResultCount()
        -- Results from before this Lua state are not ours to report.
        if not Spawn.results_seen or newest < Spawn.results_seen then Spawn.results_seen = newest end
        while Spawn.results_seen < newest do
            Spawn.results_seen = Spawn.results_seen + 1
            local id, handle, _, status = Native.SpawnResult(Spawn.results_seen)
            local req = id and Spawn.pending[id]
            if req then
                Spawn.pending[id] = nil
                if status == 1 then
                    Spawn.owner[handle] = req.ply
                else
                    Note(req.ply, string.format(STATUS[status] or "%s failed", req.name), NOTIFY_ERROR)
                end
            end
        end
    end)

    -- The proxy of a spawned entity joins its owner's cleanup list; proxies.lua calls this.
    function Spawn.ProxyCreated(ent)
        local ply = Spawn.owner[ent.gr_handle]
        if not IsValid(ply) then ply = player.GetAll()[1] end
        if IsValid(ply) then cleanup.Add(ply, "gr_rdr2", ent) end
    end

    -- The remover marks what it is about to remove: the real entity goes with the proxy.
    hook.Add("CanTool", "GR.Spawn", function(_, tr, tool)
        local ent = tr.Entity
        if tool == "remover" and IsValid(ent) and ent:GetClass() == "gr_proxy" then ent.gr_remove_real = true end
    end)

    local shutting_down = false
    hook.Add("ShutDown", "GR.Spawn", function() shutting_down = true end)

    -- A proxy that goes for any reason other than proxies.lua dropping it (out of range,
    -- gone in RDR2) takes the real entity with it if it was spawned or the remover took it:
    -- undo by entity, the cleanup menu, the remover.
    hook.Add("EntityRemoved", "GR.Spawn", function(ent)
        if shutting_down or ent:GetClass() ~= "gr_proxy" or ent.gr_dropping or not ent.gr_handle then return end
        if not (ent.gr_spawned or ent.gr_remove_real) or not GR.Native or not GR.Native.Remove then return end
        GR.Native.Remove(ent.gr_handle, 0, false)
        Spawn.owner[ent.gr_handle] = nil
    end)

    -- Cleaning up everything, or all RDR2 entities, reaches the spawned ones that are too far
    -- away to have a proxy too.
    local function RemoveAllSpawned()
        if GR.Native and GR.Native.Remove then GR.Native.Remove(0, 0, true) end
        Spawn.owner = {}
    end
    hook.Add("PostCleanupMap", "GR.Spawn", RemoveAllSpawned)
    if cleanup.CC_Cleanup then
        concommand.Add("gmod_cleanup", function(ply, command, args)
            local kind = args[1]
            if not kind or kind == "" or kind == "gr_rdr2" then RemoveAllSpawned() end
            cleanup.CC_Cleanup(ply, command, args)
        end, nil, "", { FCVAR_DONTRECORD })
    end

    -- The sandbox's amnesty.
    concommand.Add("gr_clear_wanted", function(ply)
        if IsValid(ply) and not ply:IsListenServerHost() then return end
        if GR.Native and GR.Native.ClearWanted and GR.Native.ClearWanted() then
            GR.Print("law: RDR2 is told to forget the player's crimes and bounty")
        else
            GR.Print("law: RDR2 is not connected")
        end
    end, nil, "Garry's Redemption: clear the RDR2 player's wanted level and bounty")

    concommand.Add("gr_spawn", function(ply, _, args)
        if not IsValid(ply) then ply = player.GetAll()[1] end
        if IsValid(ply) and ply:IsListenServerHost() then Spawn.Request(ply, args[1]) end
    end, nil, "Garry's Redemption: spawn an RDR2 entity by model name where the player is looking")

    return
end

-- ---- client: the Red Dead tab

language.Add("Cleanup_gr_rdr2", "RDR2 entities")
language.Add("Cleaned_gr_rdr2", "Cleaned up all RDR2 entities")
language.Add("Undone_RDR2 entity", "Undone RDR2 entity")

net.Receive("gr_spawn_note", function()
    local text = net.ReadString()
    notification.AddLegacy(text, net.ReadUInt(4), 4)
    surface.PlaySound("buttons/button10.wav")
end)

-- Source props, NPCs and vehicles are drawn into RDR2's world since milestone 9, so the
-- tabs are back by default.
local hide_tabs = CreateClientConVar("gr_hide_source_tabs", "0", true, false,
    "Garry's Redemption: 1 = hide the spawn menu tabs of things GMod cannot show over RDR2 yet")

local CATEGORIES = {
    { key = "people", name = "People", icon = "icon16/user.png" },
    { key = "horses", name = "Horses", icon = "icon16/rosette.png" },
    { key = "animals", name = "Animals", icon = "icon16/bug.png" },
    { key = "vehicles", name = "Vehicles", icon = "icon16/car.png" },
    { key = "objects", name = "Objects", icon = "icon16/box.png" },
}
local MAX_ROWS = 400
local TAB_NAME = "Red Dead"
local KEEP_TABS = { [TAB_NAME] = true, ["#spawnmenu.category.weapons"] = true }

local function SendSpawn(name)
    net.Start("gr_spawn")
    net.WriteString(name)
    net.SendToServer()
    surface.PlaySound("ui/buttonclickrelease.wav")
end

local function Data()
    if not GR.ModelData then pcall(include, "gr/models_data.lua") end
    return GR.ModelData or {}
end

local function BuildTab()
    local root = vgui.Create("DPanel")
    root:DockPadding(4, 4, 4, 4)

    local tree = vgui.Create("DTree", root)
    tree:Dock(LEFT)
    tree:SetWide(180)
    tree:DockMargin(0, 0, 4, 0)

    local right = vgui.Create("Panel", root)
    right:Dock(FILL)

    local search = vgui.Create("DTextEntry", right)
    search:Dock(TOP)
    search:DockMargin(0, 0, 0, 4)
    search:SetPlaceholderText("Search RDR2 models, or type a model name and press Enter")

    local list = vgui.Create("DListView", right)
    list:Dock(FILL)
    list:SetMultiSelect(false)
    list:AddColumn("Model")
    list:AddColumn("Kind"):SetFixedWidth(90)

    local data = Data()
    local current = nil  -- category key, nil = all

    local function Fill()
        list:Clear()
        local query = string.lower(string.Trim(search:GetValue()))
        local shown, more, exact = 0, 0, false
        for _, cat in ipairs(CATEGORIES) do
            -- A search looks through everything, whichever category is picked.
            if not current or current == cat.key or query ~= "" then
                for _, name in ipairs(data[cat.key] or {}) do
                    if query == "" or string.find(name, query, 1, true) then
                        if name == query then exact = true end
                        if shown < MAX_ROWS then
                            local line = list:AddLine(name, cat.name)
                            line.gr_name = name
                            shown = shown + 1
                        else
                            more = more + 1
                        end
                    end
                end
            end
        end
        -- The commands (commands.lua): a click runs it.
        if current == "commands" or query ~= "" then
            for _, c in ipairs(GR.Commands and GR.Commands.LIST or {}) do
                if query == "" or string.find(string.lower(c[1]), query, 1, true) then
                    local line = list:AddLine(c[1], "Command")
                    line.gr_command = c[2]
                end
            end
        end
        if current == "commands" then return end
        -- RDR2's guns (sweps.lua): a click gives the weapon instead of spawning anything.
        if current == "weapons" or query ~= "" then
            for _, gun in ipairs(GR.Guns or {}) do
                if query == "" or string.find(string.lower(gun.name), query, 1, true) then
                    local line = list:AddLine(gun.name, "Weapon")
                    line.gr_weapon = gun.class
                end
            end
        end
        if current ~= "weapons" and query ~= "" and not exact and string.match(query, "^[%w_]+$") then
            local line = list:AddLine(query, "by name")
            line.gr_name = query
        end
        if more > 0 then
            local line = list:AddLine(string.format("... %d more: search to narrow it down", more), "")
            line:SetEnabled(false)
        end
    end

    local all = tree:AddNode("Everything", "icon16/world.png")
    all.DoClick = function() current = nil Fill() end
    for _, cat in ipairs(CATEGORIES) do
        local node = tree:AddNode(string.format("%s (%d)", cat.name, #(data[cat.key] or {})), cat.icon)
        node.DoClick = function() current = cat.key Fill() end
    end
    local guns = tree:AddNode(string.format("Weapons (%d)", #(GR.Guns or {})), "icon16/gun.png")
    guns.DoClick = function() current = "weapons" Fill() end
    local commands = tree:AddNode("Commands", "icon16/application_xp_terminal.png")
    commands.DoClick = function() current = "commands" Fill() end
    if not GR.ModelData then
        tree:AddNode("No model list: run tools/gen_models.py", "icon16/error.png")
    end

    list.OnClickLine = function(lst, line)
        lst:ClearSelection()
        line:SetSelected(true)
        if line.gr_command then
            LocalPlayer():ConCommand(line.gr_command)
            surface.PlaySound("ui/buttonclickrelease.wav")
        elseif line.gr_weapon then
            RunConsoleCommand("gm_giveswep", line.gr_weapon)
            surface.PlaySound("ui/buttonclickrelease.wav")
        elseif line.gr_name then
            SendSpawn(line.gr_name)
        end
    end
    search.OnChange = function() Fill() end
    search.OnEnter = function(self)
        local name = string.lower(string.Trim(self:GetValue()))
        if string.match(name, "^[%w_]+$") then SendSpawn(name) end
    end

    current = "people"
    Fill()
    return root
end

spawnmenu.AddCreationTab(TAB_NAME, BuildTab, "icon16/world.png", -100,
    "RDR2's people, horses, animals, vehicles, objects and guns, and commands")

-- The sandbox builds its spawn menu from this list (creationmenu.lua). Filtered on the way
-- out rather than edited, so the hidden tabs come back the moment the convar is 0.
Spawn.GetCreationTabs = Spawn.GetCreationTabs or spawnmenu.GetCreationTabs
function spawnmenu.GetCreationTabs()
    local tabs = Spawn.GetCreationTabs()
    if not hide_tabs:GetBool() then return tabs end
    local kept = {}
    for name, tab in pairs(tabs) do
        if KEEP_TABS[name] then kept[name] = tab end
    end
    return kept
end

cvars.AddChangeCallback("gr_hide_source_tabs", function()
    RunConsoleCommand("spawnmenu_reload")
end, "GR.Spawn")

-- A Lua refresh after the spawn menu was built: build it again with the new tab.
if IsValid(g_SpawnMenu) then RunConsoleCommand("spawnmenu_reload") end
