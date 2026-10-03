-- Toolgun: possess one of RDR2's people, horses or animals (gr/possess.lua).

TOOL.Category = "Red Dead"
TOOL.Name = "#tool.gr_possess.name"
TOOL.Information = { { name = "left" }, { name = "right" } }

if CLIENT then
    language.Add("tool.gr_possess.name", "Possess")
    language.Add("tool.gr_possess.desc", "Become one of RDR2's people, horses or animals and walk it around")
    language.Add("tool.gr_possess.left", "Possess the person, horse or animal you point at")
    language.Add("tool.gr_possess.right", "Let go and be yourself again")
end

function TOOL:LeftClick(trace)
    if CLIENT then return true end
    local owner = self:GetOwner()
    -- While possessing, the player's eyes are inside its own proxy: look past it.
    local ent = GR.Possess.Of(owner) and GR.Possess.Target(owner) or trace.Entity
    return GR.Possess.Start(owner, ent)
end

function TOOL:RightClick()
    if CLIENT then return true end
    GR.Possess.Stop()
    return true
end

function TOOL.BuildCPanel(panel)
    panel:AddControl("Header", { Description = "#tool.gr_possess.desc" })
    panel:Help("W/A/S/D steer it relative to the camera. Sprint key: gallop or run. Walk key: walk. Jump key: jump.")
end
