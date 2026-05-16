-- Example: the full hook() surface
--
-- Every engine HOOK_* now has a Lua name and a populated ctx table.
-- A handler returning false / "stop" cancels the action — but only for
-- events whose engine call site is cancellable (see
-- docs/knowledge/workshop-hook-context-map.md for the full matrix).

-- ---- chat ----

-- Block whispers that contain a banned word.
hook("pc_whisper", function(ctx)
    -- ctx = { player, target, message }
    if ctx.message:lower():find("spamword") then
        message(ctx.player, "Message blocked.")
        return false                       -- cancel the whisper
    end
end)

-- Mirror party chat into the server log.
hook("pc_partychat", function(ctx)
    log_info(("[party] %s: %s"):format(ctx.player.name, ctx.message))
end)

-- ---- trade ----

-- Refuse trades while either side is below base level 10.
hook("trade_request", function(ctx)
    -- ctx = { player, target }
    if ctx.player.base_level < 10 or
       (ctx.target and ctx.target.base_level < 10) then
        message(ctx.player, "Trading unlocks at base level 10.")
        return "stop"
    end
end)

-- ---- guild ----

hook("guild_join", function(ctx)
    -- ctx = { player, guild_id } (informational — cannot cancel)
    announce(("%s joined guild #%d"):format(ctx.player.name, ctx.guild_id))
end)

-- ---- status changes ----

-- Log every status applied to a player; veto a specific debuff.
hook("status_change_start", function(ctx)
    -- ctx = { player?, type, val1..val4, duration_ms }
    if ctx.player then
        log_info(("SC %d on %s for %dms"):format(
            ctx.type, ctx.player.name, ctx.duration_ms))
    end
    if ctx.type == sc_id("STONE") then
        return false                       -- this player can't be petrified
    end
end)

-- ---- companions ----

hook("pet_catch", function(ctx)
    -- ctx = { player, item_id }
    log_info(("%s is trying to tame with item %d")
        :format(ctx.player.name, ctx.item_id))
end)

hook("homun_levelup", function(ctx)
    -- ctx = { new_level }   (no homunculus accessor in plugin_api yet)
    log_info("homunculus reached level " .. ctx.new_level)
end)

-- ---- skills (caster may be a mob — ctx.player only set for PCs) ----

hook("skill_use", function(ctx)
    -- ctx = { player?, skill_id, skill_lv }
    if ctx.player then
        log_info(("%s casts skill %d lv%d")
            :format(ctx.player.name, ctx.skill_id, ctx.skill_lv))
    end
end)
