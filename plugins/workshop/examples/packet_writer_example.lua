-- Example: Custom packet writer usage
--
-- This example shows how to use the packet_writer API to construct
-- binary packets and send them to players.

-- Example 1: Build and send a simple time-sync packet (0x007F)
hook("pc_login", function(ctx)
    local tick = gettick()

    -- Create a new packet
    local w = packet_writer()

    -- Write packet opcode (0x007F, little-endian)
    w:write_w(0x007F)

    -- Write current game tick (4 bytes, little-endian)
    w:write_l(tick)

    -- Send to the player
    packet_send(ctx.player, w:build())

    log_info("Sent time-sync packet 0x007F to " .. ctx.player.name)
    return true
end)

-- Example 2: Variable-length packet with length field
register_buildin("test_custom_packet", "", function(player)
    local w = packet_writer()

    -- Packet ID
    w:write_w(0x0800)  -- custom packet ID

    -- Reserve space for length field (write as 0 first)
    w:write_w(0)

    -- Write some data
    w:write_l(player.aid)           -- account ID
    w:write_str(player.name, 24)    -- name (fixed 24 bytes)
    w:write_str("Custom message!")  -- variable-length string

    -- Patch the length field at offset 2
    w:set_w(2, w:len())

    -- Send to the player
    packet_send(player, w:build())

    mes(player, "Custom packet sent!")
    close_dialog(player)
    return true
end)

-- Example 3: Building a packet with multiple field types
hook("pc_chat", function(ctx)
    -- Only log "debug" prefix messages
    if not string.match(ctx.message, "^debug ") then
        return true
    end

    local w = packet_writer()

    w:write_w(0x0801)           -- custom chat packet
    w:write_l(ctx.player.aid)   -- sender account ID
    w:write_b(1)                -- message type (1 = debug)
    w:write_w(ctx.player.x)     -- sender X coordinate
    w:write_w(ctx.player.y)     -- sender Y coordinate
    w:write_zeros(4)            -- reserved field
    w:write_str(ctx.message:sub(7))  -- message (skip "debug " prefix)

    -- Send to all players in area
    packet_send(ctx.player, w:build(), 2)  -- 2 = AREA

    return false  -- suppress normal chat
end)

-- Example 4: Patching/updating packet fields
local function build_status_packet(player)
    local w = packet_writer()

    w:write_w(0x0802)           -- status packet ID
    w:write_b(0)                -- version byte (placeholder)
    w:write_l(player.aid)
    w:write_b(player.base_level)
    w:write_b(player.job_level)
    w:write_l(os.time())        -- timestamp

    -- Can patch fields after writing if needed
    -- w:set_b(2, 1)  -- update version

    return w:build()
end

register_atcmd("statuspacket", function(player)
    local data = build_status_packet(player)
    packet_send(player, data)
    message(player, "Status packet sent!")
    return true
end)
