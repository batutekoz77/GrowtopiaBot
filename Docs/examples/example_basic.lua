local bot = getBot()
print("bot:", bot.name, "status:", bot.status, "world:", bot.world)

if not bot.online then
    bot:connect()
    if not bot:waitForOnline(60000) then
        warn("could not log in: " .. bot.statusText)
        return
    end
end

if bot:warpAndWait("START", 20000) then
    bot:say("hello from GrowtopiaBot!")
    print(string.format("in %s at tile %d,%d with %d gems", bot.world, bot.tileX, bot.tileY, bot.gems))
else
    warn("warp failed, still in: " .. bot.world)
end

for _, item in ipairs(bot:getInventory()) do
    print("item", item.id, "x" .. item.count)
end
