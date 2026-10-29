local ITEM = 242
local KEEP = 0
local THRESHOLD = 50
local DEPOSIT = "MYDEPOSIT"
local DROP_TILE = { x = 10, y = 20 }

local bot = getBot()

local function deposit()
    local count = bot:getItemCount(ITEM) - KEEP
    if count <= 0 then return end
    local home = bot.world
    if not bot:warpAndWait(DEPOSIT, 20000) then
        warn("can't reach " .. DEPOSIT)
        return
    end
    local ok, why = bot:walkAndWait(DROP_TILE.x, DROP_TILE.y)
    if not ok then
        warn("can't walk to the drop tile: " .. why)
        return
    end
    bot:drop(ITEM, count)
    log(string.format("%s dropped %d x %d in %s", bot.name, count, ITEM, DEPOSIT))
    sleep(1000)
    if home ~= "" then bot:warpAndWait(home, 20000) end
end

bot:on("item", function(b, id, delta, amount)
    if id == ITEM and delta > 0 then print("got", delta, "now", amount) end
end)

while true do
    if bot.inWorld and bot:getItemCount(ITEM) >= THRESHOLD then deposit() end
    sleep(5000)
end
