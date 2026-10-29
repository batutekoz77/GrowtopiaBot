on("console", function(bot, text)
    if text:find("Collected") then print(bot.name, "->", text) end
end)

on("talk", function(bot, netID, name, text)
    if text:lower() == "!ping" then bot:say("pong, " .. name) end
end)

on("moderator", function(bot, name)
    warn(bot.name .. ": moderator " .. name .. " joined, leaving the world")
    bot:leave()
end)

on("disconnected", function(bot, reason)
    log(bot.name .. " disconnected: " .. reason)
end)

on("world_enter", function(bot, world, width, height)
    log(string.format("%s entered %s (%dx%d)", bot.name, world, width, height))
end)

listen()
