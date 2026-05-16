-- Example: on_event / npc_event_all — rAthena NPC-script events in Lua
--
-- on_event(label, fn) binds fn to ANY rAthena NPC-script event label —
-- the same labels .txt scripts use. fn(label, ctx); ctx = { event, player? }.
-- Handlers that only read the first arg keep working.
--
-- on_event is informational: returning false does NOT cancel. To gate an
-- action use the cancellable hook() event instead.

-- ---- script_config PC labels (ctx.player is set) ----
-- Default names live in struct script_config (src/map/script.cpp). They
-- follow conf/script.conf renames automatically.

on_event("OnPCLoginEvent", function(label, ctx)
    if ctx.player then
        message(ctx.player, "Welcome back, " .. ctx.player.name .. "!")
    end
end)

on_event("OnPCDieEvent", function(label, ctx)
    if ctx.player then
        log_info(ctx.player.name .. " died")
    end
end)

on_event("OnNPCKillEvent", function(label, ctx)
    -- fired when a player kills a mob (NPCE_KILLNPC).
    -- NOTE: set_var/get_var need an active script_state (register_buildin
    -- only). From an on_event handler use script_eval, which runs under
    -- the engine fake NPC with the player attached as rid.
    if ctx.player then
        script_eval([[ set #kills, #kills + 1; ]], ctx.player)
    end
end)

-- ---- broadcast labels (no player) ----

on_event("OnInit", function()
    log_info("all NPCs loaded — workshop ready")
end)

on_event("OnInterIfInit", function()
    log_info("inter-server (char-server) connection established")
end)

on_event("OnAgitStart", function()
    announce("War of Emperium has begun!", 0)
end)

-- ---- clock labels ----
-- These now come from the engine clock dispatcher (the workshop's own
-- ticker was removed), so they fire exactly once, in sync with .txt NPCs.

on_event("OnClock0000", function()
    rathena([[ announce "Midnight reset.", bc_all; ]])
end)

-- Typed shortcuts synthesise the same label strings:
on_minute(30, function()
    log_info("half-past the hour")
end)

on_hour(12, function()
    announce("It is noon.", 0)
end)

-- ---- broadcasting a label to every NPC (donpcevent) ----

register_atcmd("startevent", 60, function(player)
    local n = npc_event_all("OnServerEvent")          -- no rid
    message(player, ("Triggered OnServerEvent on %d NPCs"):format(n))
    return 1
end)

register_atcmd("myevent", 0, function(player)
    npc_event_all("OnPlayerEvent", player)            -- attach player rid
    return 1
end)

-- ---- re-apply runtime registrations on char-server (re)connect ----
-- The char-server resends the storage list on every (re)connect, wiping
-- runtime register_storage entries. intif_connected is the place to
-- re-apply them; ctx.first is true only on the initial connect.

hook("intif_connected", function(ctx)
    register_storage(20, "Event Vault", 600, "event_storage")
    if not ctx.first then
        log_info("re-registered Event Vault after char-server reconnect")
    end
end)
