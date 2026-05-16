-- Example: script_eval / rathena — call any rAthena system from Lua
--
-- script_eval(src [, player]) compiles and runs an rAthena script
-- snippet synchronously under the engine fake NPC. With a player it is
-- attached as the script rid (rid2sd / getcharid / getitem target them).
-- `rathena` is an alias. Returns true if it parsed and ran.
--
-- Use this for subsystems with no typed wrapper (mail, instance, clan,
-- channel, battleground, achievement, ...). It runs with full script
-- authority — never build a snippet from raw client input.
--
-- HARD RULE: the snippet must NOT suspend. No sleep / sleep2, no dialog
-- (mes+next / menu / input). For sleeping work use register_buildin.

-- 1. Reach a subsystem the workshop API does not wrap: send mail.
register_atcmd("gift", 0, function(player)
    script_eval([[
        mail getcharid(0),
            "Server", "Daily Gift", "Thanks for playing!",
            10000, 501, 5;
    ]], player)
    message(player, "A gift is waiting in your mailbox.")
    return 1
end)

-- 2. No player attached (rid 0) — server-wide script command.
on_event("OnInit", function()
    rathena([[ announce "Server scripts loaded.", bc_all; ]])
end)

-- 3. Build the snippet from Lua values (string.format), still trusted.
local function grant_title(player, quest_id)
    script_eval(("setquest %d;"):format(quest_id), player)
end

hook("pc_baselevelup", function(ctx)
    if ctx.player and ctx.player.base_level == 99 then
        grant_title(ctx.player, 60100)          -- "reached 99" quest log
        message(ctx.player, "Title unlocked.")
    end
end)

-- 4. Mix with the typed API: typed call for the hot read, eval for the
--    rare side-effect.
register_buildin("instance_warp", "s", function(player, inst_name)
    -- (typed lookups would go here for any hot path)
    script_eval(([[
        .@id = instance_create("%s", getcharid(3), IM_PARTY);
        instance_attachmap("1@dummy", .@id);
        instance_init(.@id);
    ]]):format(inst_name), player)
    return 1
end)
