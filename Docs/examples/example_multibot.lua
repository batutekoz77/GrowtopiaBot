local WORLD = "START"
local GAP_MS = 3000

for i, bot in ipairs(getBots()) do
    if not bot.online then
        bot:connect()
        sleep(GAP_MS)
    end
end

for _, bot in ipairs(getBots()) do
    if bot:waitForOnline(60000) then
        bot:setTargetWorld(WORLD)
    else
        warn(bot.growid .. " is not online: " .. bot.statusText)
    end
end

sleep(10000)
local total = 0
for _, bot in ipairs(getBots()) do
    total = total + bot.gems
    print(string.format("%-16s %-12s %-10s %8d gems  ping %d ms", bot.name, bot.status, bot.world, bot.gems, bot.ping))
end
print("total gems:", total)
