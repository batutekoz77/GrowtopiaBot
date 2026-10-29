local API_BASE_URL        = "http://localhost/api/deposit.php"
local API_KEY             = "YOUR_API_KEY_HERE"
local DISCORD_WEBHOOK_URL = "https://discord.com/api/webhooks/1564662657368330272/sIOXkPgD3c5UIt04NwVCOxiwSanyhhWVFv-dkLmmhVLHyLzm3yNP_JLwauCXpSFB9xL1"
local STORAGE_WORLDS = {
    { world = "SGHG84J", box_x = 70, box_y = 23, side = "left", door = "" },
}
local ITEM_WL             = 242
local ITEM_DL             = 1796
local ITEM_BGL            = 7188
local POLL_INTERVAL       = 5000
local TIMEOUT_SECONDS     = 180
local HEARTBEAT_EVERY     = 30
local DROP_SETTLE_MS      = 1500
local WARP_SETTLE_MS      = 10000
local CONNECT_TIMEOUT_MS  = 35000
local BREAK_TOTAL_HITS    = 400
local BREAK_HIT_DELAY     = 180
local STEP_DELAY          = 250
local HTTP_TIMEOUT_S      = 15

local WL_PER_DL           = 100
local DL_PER_BGL          = 100

local PACKET_STATE                = 0
local PACKET_ITEM_ACTIVATE_OBJECT = 11
local PACKET_ITEM_CHANGE_OBJECT   = 14

local bot = getBot()
bot:setAutoReconnect(false)

local function dlog(fmt, ...)
    log("[DEP] " .. string.format(fmt, ...))
end

local function nowSec()
    return millis() / 1000
end

local TMP = (os.getenv("TEMP") or ".") .. "\\growtopia_dep_" .. scriptId()

local function readFile(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local s = f:read("a")
    f:close()
    return s
end

local function writeFile(path, data)
    local f = assert(io.open(path, "wb"))
    f:write(data)
    f:close()
end

local function httpRequest(method, url, headers, body)
    local reqPath, resPath, hdrPath = TMP .. "_req.json", TMP .. "_res.txt", TMP .. "_hdr.txt"
    os.remove(resPath)
    os.remove(hdrPath)

    local args = {
        "curl.exe -s -S -m " .. HTTP_TIMEOUT_S,
        "-X " .. method,
        '-o "' .. resPath .. '"',
        '-D "' .. hdrPath .. '"',
    }
    for k, v in pairs(headers or {}) do
        args[#args + 1] = '-H "' .. k .. ": " .. v .. '"'
    end
    if body then
        writeFile(reqPath, body)
        args[#args + 1] = '--data-binary "@' .. reqPath .. '"'
    end
    args[#args + 1] = '"' .. url .. '"'

    local _, _, code = os.execute('"' .. table.concat(args, " ") .. '"')

    local status = 0
    for s in (readFile(hdrPath) or ""):gmatch("HTTP/[%d%.]+ (%d+)") do
        status = tonumber(s)
    end
    if status == 0 then
        return nil, "curl exit " .. tostring(code)
    end
    return status, readFile(resPath) or ""
end

local function jsonEscape(s)
    s = tostring(s or "")
    s = s:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\r", ""):gsub("\n", "\\n")
    return s
end

local function field(raw, name)
    if not raw then return nil end
    local v = raw:match('"' .. name .. '"%s*:%s*"([^"]*)"')
    if v then return v end
    return raw:match('"' .. name .. '"%s*:%s*(%-?%d+%.?%d*)')
end

local function encodeBody(body)
    local parts = {}
    for k, v in pairs(body) do
        local vt = type(v)
        if vt == "string" then
            parts[#parts + 1] = '"' .. k .. '":"' .. jsonEscape(v) .. '"'
        elseif vt == "number" or vt == "boolean" then
            parts[#parts + 1] = '"' .. k .. '":' .. tostring(v)
        end
    end
    return "{" .. table.concat(parts, ",") .. "}"
end

local function sendDiscordWebhook(title, description, color)
    if not DISCORD_WEBHOOK_URL or DISCORD_WEBHOOK_URL == "" or DISCORD_WEBHOOK_URL:find("BURAYA") then
        return
    end
    local botName = (bot.name ~= "" and bot.name) or bot.growid
    local payload = string.format(
        '{"username":"GrowtopiaBot Deposit","embeds":[{"title":"%s","description":"%s","color":%d,"footer":{"text":"GrowtopiaBot Deposit Bot • Bot: %s"}}]}',
        jsonEscape(title), jsonEscape(description), tonumber(color) or 3066993, jsonEscape(botName))

    local status, res = httpRequest("POST", DISCORD_WEBHOOK_URL .. "?wait=true", {
        ["Content-Type"] = "application/json",
        ["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) GrowtopiaBot/2.1",
    }, payload)

    if not status then
        dlog("[WEBHOOK] Istek basarisiz: %s", tostring(res))
    elseif status == 200 or status == 204 then
        dlog("[WEBHOOK] Bildirim Discord'a gonderildi.")
    else
        dlog("[WEBHOOK] Discord hatasi! Status: %d | Cevap: %s", status, tostring(res))
    end
end

local function request(action, body)
    local payload = body and encodeBody(body) or nil
    local headers = {
        ["Authorization"] = "Bearer " .. API_KEY,
        ["Accept"] = "application/json",
    }
    if payload then headers["Content-Type"] = "application/json" end

    for attempt = 1, 3 do
        local status, res = httpRequest(payload and "POST" or "GET", API_BASE_URL .. "?action=" .. action, headers, payload)
        if status == 200 then
            return res
        end
        if status and status < 500 then
            dlog("%s -> %d: %s", action, status, tostring(res))
            return nil
        end
        if attempt < 3 then
            dlog("%s basarisiz (deneme %d: %s), tekrar...", action, attempt, status and ("HTTP " .. status) or tostring(res))
            sleep(1500 * attempt)
        end
    end

    dlog("%s: API'ye ulasilamadi", action)
    return nil
end

local function randomWorldName()
    local chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    local out = {}
    for i = 1, 12 do
        local n = math.random(1, #chars)
        out[i] = chars:sub(n, n)
    end
    return table.concat(out)
end

local function normalizeName(name)
    return (tostring(name or ""):gsub("`.", ""):gsub("%s+", ""):lower())
end

local function myName()
    return normalizeName(bot.name ~= "" and bot.name or bot.growid)
end

local function safeDisconnect()
    bot:setAutoReconnect(false)
    local st = bot.status
    if st == "offline" or st == "stopped" or st == "idle" then
        return
    end
    dlog("Sunucudan cikis yapiliyor (disconnect)...")
    bot:disconnect()
    bot:waitForStatus("offline", 5000)
end

local function ensureConnected()
    if bot.online then
        return true
    end

    dlog("Siparis icin bot oyuna baglaniyor (Mevcut durum: %s)...", bot.status)
    bot:setAutoReconnect(true)
    bot:connect()

    local deadline = millis() + CONNECT_TIMEOUT_MS
    sleep(500)
    while millis() < deadline do
        if bot.online then
            dlog("Baglanti basarili! Sunucunun oturmasi icin bekleniyor...")
            sleep(2500)
            return true
        end
        local why = bot.statusText:lower()
        if bot.status == "stopped" or why:find("update") or why:find("maintenance")
            or why:find("2fa") or why:find("ban") or why:find("suspend") then
            dlog("Kritik baglanti engeli: %s (%s)", bot.status, bot.statusText)
            safeDisconnect()
            return false
        end
        sleep(500)
    end

    dlog("Baglanti zaman asimina ugradi (Durum: %s - %s)", bot.status, bot.statusText)
    safeDisconnect()
    return false
end

local function enterWorld(world, door)
    world = world:upper()
    for attempt = 1, 3 do
        if not bot.online and not ensureConnected() then
            return false
        end
        if bot:warpAndWait(world, WARP_SETTLE_MS, door or "") then
            sleep(1000)
            return true
        end
        dlog("Dunyaya girilemedi (deneme %d) - Mevcut Dunya: %s", attempt, bot.world ~= "" and bot.world or "EXIT")
        sleep(1500)
    end
    return false
end

local function getInventoryTotalWL()
    local wl  = bot:getItemCount(ITEM_WL)
    local dl  = bot:getItemCount(ITEM_DL)
    local bgl = bot:getItemCount(ITEM_BGL)
    return (bgl * DL_PER_BGL * WL_PER_DL) + (dl * WL_PER_DL) + wl, bgl, dl, wl
end

local function formatLockBreakdown(diffBGL, diffDL, diffWL, totalGainWL)
    local parts = {}
    if diffBGL > 0 then parts[#parts + 1] = diffBGL .. " BGL" end
    if diffDL > 0 then parts[#parts + 1] = diffDL .. " DL" end
    if diffWL > 0 then parts[#parts + 1] = diffWL .. " WL" end

    if #parts == 0 then
        local rem = totalGainWL
        local b = rem // 10000
        rem = rem % 10000
        local d = rem // 100
        local w = rem % 100
        if b > 0 then parts[#parts + 1] = b .. " BGL" end
        if d > 0 then parts[#parts + 1] = d .. " DL" end
        if w > 0 then parts[#parts + 1] = w .. " WL" end
    end
    return table.concat(parts, ", ")
end

local function compressLocks()
    if bot:getItemCount(ITEM_WL) >= 100 and bot:getItemCount(ITEM_DL) < 200 then
        dlog("100 WL -> 1 DL donusturuluyor...")
        bot:wear(ITEM_WL)
        sleep(600)
    end
end

local function walkTo(tx, ty)
    for _ = 1, 120 do
        local x, y = bot.tileX, bot.tileY
        if x == tx and y == ty then return true end
        if not bot.inWorld then return false end
        if x ~= tx then
            x = x + (tx > x and 1 or -1)
        else
            y = y + (ty > y and 1 or -1)
        end
        bot:walk(x, y)
        sleep(STEP_DELAY)
    end
    return bot.tileX == tx and bot.tileY == ty
end

local function setBotFacing(faceLeft)
    bot:sendPacket({
        type = PACKET_STATE, x = bot.x, y = bot.y, punchX = -1, punchY = -1,
        flags = faceLeft and 0x30 or 0x20, reliable = false,
    })
    sleep(350)
end

local objects = {}
local objectCounter = 0

local function resetObjects()
    objects = {}
    objectCounter = 0
end

local function onPacket(_, ptype, netID, value, x, y)
    if ptype ~= PACKET_ITEM_CHANGE_OBJECT or netID ~= -1 then return end
    objectCounter = objectCounter + 1
    objects[#objects + 1] = { oid = objectCounter, id = value, x = x, y = y, tries = 0 }
end

local function isLock(id)
    return id == ITEM_WL or id == ITEM_DL or id == ITEM_BGL
end

local function collectDroppedLocks()
    local found = false
    local keep = {}
    for _, obj in ipairs(objects) do
        if isLock(obj.id) and obj.tries < 5 then
            found = true
            obj.tries = obj.tries + 1
            local ox, oy = math.floor((obj.x + 10) / 32), math.floor((obj.y + 10) / 32)
            if math.abs(ox - bot.tileX) > 1 or math.abs(oy - bot.tileY) > 1 then
                bot:walk(ox, oy)
                sleep(STEP_DELAY)
            end
            bot:sendPacket({ type = PACKET_ITEM_ACTIVATE_OBJECT, x = obj.x, y = obj.y, value = obj.oid })
            keep[#keep + 1] = obj
        end
    end
    objects = keep
    if found then sleep(200) end
    return found
end

local function dropItemIntoBox(itemId, count, store)
    if count <= 0 or bot:getItemCount(itemId) < count then return true end
    local startCount = bot:getItemCount(itemId)

    local faceLeft = store.side == "right"
    local standX = faceLeft and (store.box_x + 1) or (store.box_x - 1)
    if bot.tileX ~= standX or bot.tileY ~= store.box_y then
        dlog("Display Box yanina geciliyor -> (%d, %d)", standX, store.box_y)
        walkTo(standX, store.box_y)
    end
    setBotFacing(faceLeft)

    for attempt = 1, 4 do
        if attempt == 2 then
            faceLeft = not faceLeft
            standX = faceLeft and (store.box_x + 1) or (store.box_x - 1)
            dlog("Alternatif yan kareye geciliyor (%d, %d)...", standX, store.box_y)
            walkTo(standX, store.box_y)
            setBotFacing(faceLeft)
        elseif attempt == 3 then
            setBotFacing(bot.tileX > store.box_x)
        elseif attempt == 4 then
            setBotFacing(not faceLeft)
        end

        bot:drop(itemId, count)
        sleep(1300)

        if bot:getItemCount(itemId) < startCount then
            return true
        end
        dlog("Drop denemesi %d/4 basarisiz oldu, pozisyon yenileniyor...", attempt)
        sleep(400)
    end
    return false
end

local function storeItemsToDisplayBox()
    dlog("Depo dunyalarina esya aktarimi baslatiliyor...")
    compressLocks()

    local bglCount = bot:getItemCount(ITEM_BGL)
    local dlCount  = bot:getItemCount(ITEM_DL)
    local wlCount  = bot:getItemCount(ITEM_WL)
    local wlToStore = math.max(0, wlCount - 1)

    if bglCount == 0 and dlCount == 0 and wlToStore == 0 then
        dlog("Depolanacak yeni esya bulunmuyor.")
        return
    end

    local stored = false
    for _, store in ipairs(STORAGE_WORLDS) do
        dlog("Depo dunyasina baglaniliyor: %s", store.world)
        if enterWorld(store.world, store.door) then
            sleep(800)
            local allDropped = true
            for _, entry in ipairs({ { ITEM_BGL, bglCount, "BGL" }, { ITEM_DL, dlCount, "DL" }, { ITEM_WL, wlToStore, "WL" } }) do
                local id, count, label = entry[1], entry[2], entry[3]
                if count > 0 then
                    if dropItemIntoBox(id, count, store) then
                        dlog("%d adet %s Display Box'a birakildi.", count, label)
                    else
                        dlog("HATA: %d adet %s atilamadi!", count, label)
                        allDropped = false
                    end
                end
            end
            if allDropped then
                stored = true
                sleep(1000)
                break
            end
            dlog("UYARI: Esyalarin tamami bu depoya birakilamadi!")
        else
            dlog("UYARI: %s dunyasina girilemedi, siradaki depo deneniyor...", store.world)
        end
    end

    if not stored then
        dlog("KRITIK HATA: Esyalar depoya birakilamadi, botun ustunde kaldi!")
        sendDiscordWebhook("⚠️ Depolama Hatası", "Eşyalar Display Box'a atılamadı!\nEşyalar botun üstünde kaldı.", 15158332)
    end
end

local function breakAndRecoverWorldLock(baseX, baseY, lockX, lockY, beforeBreakWL)
    dlog("Kilit kiriliyor (Hedef Kare: %d, %d | Max %d vurus)...", lockX, lockY, BREAK_TOTAL_HITS)
    local target = beforeBreakWL + 1

    if bot.tileX ~= baseX or bot.tileY ~= baseY then
        walkTo(baseX, baseY)
    end

    for hit = 1, BREAK_TOTAL_HITS do
        if not bot.inWorld then
            dlog("Kilit kirilirken dunyadan dusuldu!")
            return false
        end
        if getInventoryTotalWL() >= target then
            dlog("Kilit %d. vurusta kirildi ve envantere alindi.", hit - 1)
            return true
        end

        bot:punch(lockX, lockY)
        sleep(BREAK_HIT_DELAY)

        if hit % 6 == 0 then
            collectDroppedLocks()
        end
    end

    local deadline = millis() + 4000
    while millis() < deadline do
        collectDroppedLocks()
        sleep(250)
        if getInventoryTotalWL() >= target then
            dlog("%d vurus sonunda kilit geri alindi.", BREAK_TOTAL_HITS)
            return true
        end
    end
    dlog("UYARI: %d vurus yapildi ancak kilit geri alinamadi!", BREAK_TOTAL_HITS)
    return false
end

local function fetchNextOrder()
    local raw = request("next-pending")
    if not raw or field(raw, "status") ~= "claimed" then
        return nil
    end
    local growid = field(raw, "growid")
    local orderId = field(raw, "order_id")
    if not growid or growid == "" or not orderId or orderId == "0" then
        return nil
    end
    return {
        growid   = growid,
        order_id = orderId,
        code     = field(raw, "code") or "",
        in_queue = tonumber(field(raw, "in_queue") or "0") or 0,
        reported = false,
    }
end

local function failOrder(order, reason)
    if order.reported then return end
    order.reported = true
    dlog("BASARISIZ: %s (%s)", reason, order.growid)
    request("failed", { order_id = order.order_id, growid = order.growid, reason = reason })
    sendDiscordWebhook("❌ Deposit Başarısız / İptal",
        string.format("**Oyuncu:** %s\n**Kod:** %s\n**Sebep:** %s", order.growid, order.code, reason), 15158332)
end

local handlers = {}

local function removeHandlers()
    for _, id in ipairs(handlers) do off(id) end
    handlers = {}
end

local function handleOrder(order)
    local orderId = order.order_id
    local target  = order.growid
    dlog("SIPARIS ALINDI: %s | Oyuncu: %s | Kuyruk: %d", order.code, target, order.in_queue)

    if not ensureConnected() then
        failOrder(order, "BOT_CONNECT_FAILED")
        return safeDisconnect()
    end

    compressLocks()
    local startTotalWL, _, startDL, startWL = getInventoryTotalWL()
    if startWL < 1 then
        failOrder(order, "NO_WORLD_LOCK")
        return safeDisconnect()
    end
    if startWL > 190 and startDL > 190 then
        failOrder(order, "INVENTORY_FULL")
        return safeDisconnect()
    end

    local world = randomWorldName()
    dlog("Dunya kuruluyor: %s", world)
    resetObjects()
    handlers[#handlers + 1] = bot:on("packet", onPacket)

    if not enterWorld(world) then
        failOrder(order, "WARP_FAILED")
        return safeDisconnect()
    end

    local others = bot:getPlayers()
    if #others > 0 then
        dlog("Guvenlik: Dunya bos degil (%s var), iptal ediliyor.", others[1].name)
        failOrder(order, "WORLD_NOT_EMPTY")
        return safeDisconnect()
    end

    local baseX, baseY = bot.tileX, bot.tileY
    local lockX, lockY = baseX, baseY - 1

    local placedOk = false
    for placeTry = 1, 3 do
        bot:place(lockX, lockY, ITEM_WL)
        sleep(800)
        if bot:getItemCount(ITEM_WL) < startWL then
            placedOk = true
            break
        end
        dlog("Kilit yerlesmedi (deneme %d), tekrar deneniyor...", placeTry)
        sleep(400)
    end
    if not placedOk then
        failOrder(order, "LOCK_PLACE_FAILED")
        return safeDisconnect()
    end

    local postLockTotalWL, postBGL, postDL, postWL = getInventoryTotalWL()

    request("set-world", { order_id = orderId, growid = target, world = world })
    dlog("%s icin %s hazir. Oyuncu bekleniyor...", target, world)

    local wanted = normalizeName(target)
    local self = myName()
    local banned = {}
    handlers[#handlers + 1] = bot:on("player_join", function(b, _, name)
        local n = normalizeName(name)
        if n ~= wanted and n ~= self and not banned[n] then
            banned[n] = true
            dlog("Yabanci oyuncu tespit edildi ve banlandi: %s", n)
            b:say("/ban " .. n)
        end
    end)

    local started  = nowSec()
    local lastBeat = nowSec()
    local depositDetected = false
    local playerGainWL = 0
    local gainedBGL, gainedDL, gainedWL = 0, 0, 0

    while nowSec() - started < TIMEOUT_SECONDS do
        if not bot:isInWorld(world) then
            dlog("UYARI: Bot dunyadan dustu!")
            break
        end

        sleep(400)
        local sawDrop = collectDroppedLocks()
        local currentNetGain = getInventoryTotalWL() - postLockTotalWL

        if sawDrop or currentNetGain > 0 then
            dlog("Drop algilandi, oyuncunun diger droplari atmasi icin bekleniyor...")
            local settleUntil = millis() + DROP_SETTLE_MS
            while millis() < settleUntil do
                sleep(200)
                collectDroppedLocks()
            end

            bot:say("/ban " .. wanted)
            sleep(400)
            collectDroppedLocks()
            sleep(400)

            local curTotal, curBGL, curDL, curWL = getInventoryTotalWL()
            currentNetGain = curTotal - postLockTotalWL
            if currentNetGain > 0 then
                depositDetected = true
                playerGainWL = currentNetGain
                gainedBGL = math.max(0, curBGL - postBGL)
                gainedDL  = math.max(0, curDL - postDL)
                gainedWL  = math.max(0, curWL - postWL)
                dlog("Drop alindi (+%d WL).", playerGainWL)
            else
                dlog("SAHTE DROP DENEMESI veya toplanamadi (Net Kazanc: 0)")
            end
            break
        end

        if nowSec() - lastBeat >= HEARTBEAT_EVERY then
            request("heartbeat", { order_id = orderId })
            lastBeat = nowSec()
        end
    end

    order.reported = true
    if depositDetected then
        local breakdownText = formatLockBreakdown(gainedBGL, gainedDL, gainedWL, playerGainWL)
        dlog("BASARILI DEPOSIT! Alinan: %s (Toplam: +%d WL)", breakdownText, playerGainWL)
        sendDiscordWebhook("✅ Başarılı Deposit", string.format(
            "**Oyuncu:** %s\n**Alınan Eşya:** %s\n**Toplam Değer:** %d WL\n**Dünya:** %s\n**Sipariş Kodu:** %s",
            target, breakdownText, playerGainWL, world, order.code), 3066993)
        request("completed", { order_id = orderId, growid = target, wl_total = playerGainWL })
    else
        dlog("ZAMAN ASIMI VEYA GECERSIZ DROP: Envantere kilit girmedi.")
        request("timeout", { order_id = orderId, growid = target, reason = "NO_LOCK_DROPPED" })
        sendDiscordWebhook("⏰ Deposit Zaman Aşımı", string.format(
            "**Oyuncu:** %s\n**Dünya:** %s\n**Sebep:** Süre doldu veya geçerli kilit atılmadı.", target, world), 16776960)
    end

    if bot:isInWorld(world) then
        local ok, err = pcall(breakAndRecoverWorldLock, baseX, baseY, lockX, lockY, getInventoryTotalWL())
        if not ok then dlog("Kilit kirma hatasi: %s", tostring(err)) end
    end
    removeHandlers()

    if depositDetected then
        storeItemsToDisplayBox()
    end

    safeDisconnect()
    dlog("Islem tamamlandi, siradaki deposit bekleniyor...")
    return playerGainWL
end

dlog("GrowtopiaBot deposit botu baslatildi (On-Demand Connect Modu). Bot: %s", tostring(bot))
safeDisconnect()

while true do
    if bot.online then
        safeDisconnect()
    end

    local ok, order = pcall(fetchNextOrder)
    if not ok then
        dlog("Kuyruk sorgusu hata verdi: %s", tostring(order))
        sleep(POLL_INTERVAL)
    elseif not order then
        sleep(POLL_INTERVAL)
    else
        local done, err = pcall(handleOrder, order)
        removeHandlers()
        if not done then
            dlog("KRITIK HATA: %s", tostring(err))
            if not order.reported then
                pcall(request, "failed", { order_id = order.order_id, growid = order.growid, reason = "BOT_INTERNAL_ERROR" })
            end
            safeDisconnect()
            sleep(3000)
        end
    end
end
