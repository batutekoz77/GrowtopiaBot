# GrowtopiaBot Lua API

GrowtopiaBot, botları **Lua 5.4** scriptleriyle yönetmeni sağlar: bağlanma / kopma, warp, konuşma,
drop, yürüme, punch/place, ham paket gönderme ve botun tüm bilgilerini (isim, envanter, world,
oyuncular, gems, ping...) okuma. Oyun içi olaylara (`console`, `talk`, `world_enter`,
`disconnected`...) handler bağlayabilirsin.

- Scriptler `GrowtopiaBot.exe` yanındaki **`scripts/`** klasöründe durur (`.lua`).
- Paneldeki **Scripts** sekmesinden yazılır, kaydedilir (Ctrl+S), yüklenir ve çalıştırılır (F5).
- Aynı sekmenin altındaki **Lua >** satırı tek satırlık komut çalıştırır: `getBot().gems`
  yazarsan sonucu basar.
- Örnekler: `Docs/examples/` (build sırasında `scripts/` içine kopyalanır).

---

## İçindekiler

1. [Nasıl çalışır](#1-nasıl-çalışır)
2. [Global fonksiyonlar](#2-global-fonksiyonlar)
3. [Bot nesnesi — özellikler](#3-bot-nesnesi--özellikler)
4. [Bot nesnesi — metotlar](#4-bot-nesnesi--metotlar)
5. [Olaylar (events)](#5-olaylar-events)
6. [Durum (status) değerleri](#6-durum-status-değerleri)
7. [Örnekler](#7-örnekler)
8. [Performans notları ve sınırlar](#8-performans-notları-ve-sınırlar)

---

## 1. Nasıl çalışır

- Her script **kendi thread'inde ve kendi Lua state'inde** çalışır. Bir scriptin sonsuz
  döngüsü ne botları ne paneli yavaşlatır; aynı anda istediğin kadar script çalışabilir.
- Bot komutları (`warp`, `say`, `drop`...) **sıraya alınır** ve botun kendi thread'inde
  çalışır; fonksiyon hemen döner. Sonucunu beklemek için `waitFor...` fonksiyonlarını
  ya da `wait()`'i kullan.
- Okumalar (`bot.world`, `bot.gems`...) her zaman **o anki** değeri verir (kilitli, thread-safe).
- **Olay handler'ları** (`on(...)`) sadece script `sleep()`, `wait()`, `listen()` veya bir
  `waitFor...` içindeyken çalışır. Yani handler'lar ana kodunla aynı anda koşmaz — kilit,
  race condition derdi yok.
- Script durdurulduğunda (panelde **Stop** veya `stopScript(id)`) çalışan kod nerede olursa olsun
  (sonsuz döngü, `sleep`, `pcall` içi dahil) birkaç milisaniye içinde kesilir.
- `getBot()` argümansız çağrılınca, **panelde seçili olan** botu döndürür (yoksa listedeki ilk bot).
  "Run for every bot" butonu scripti her bot için ayrı ayrı başlatır; her kopyada
  `getBot()` o botu verir.

Hata olursa script durur, hata satır numarasıyla birlikte **Output** paneline ve loga yazılır.

---

## 2. Global fonksiyonlar

### Çıktı

| Fonksiyon | Açıklama |
|---|---|
| `print(...)` | Script Output paneline yazar (değerler tab ile ayrılır). |
| `log(...)` | Output'a **ve** ana Log sekmesine yazar (tag: `LUA:<script>`). |
| `warn(...)` | `log` gibi ama uyarı (sarı) olarak. |

### Zaman ve akış

| Fonksiyon | Dönüş | Açıklama |
|---|---|---|
| `sleep(ms)` | – | `ms` milisaniye bekler; beklerken olay handler'larını çalıştırır. |
| `millis()` | integer | Script başladığından beri geçen ms. |
| `listen([ms])` | – | Sadece olayları işleyerek bekler. Argümansız: script durdurulana kadar. |
| `wait(event, [timeoutMs], [bot])` | `bot, değerler...` veya `nil` | Bir olayı bekler (aşağıya bak). `timeoutMs` yok/negatif = sonsuz. |
| `exit()` | – | Scripti hatasız bitirir. |
| `scriptId()` | integer | Bu scriptin id'si. |
| `scriptName()` | string | Bu scriptin adı (dosya adı). |

`pcall` / `xpcall` normal Lua'daki gibi çalışır; tek fark, scriptin **durdurulmasını**
yakalayamazlar (böylece `while true do pcall(sleep, 100) end` bile durdurulabilir).

### Botlara erişim

| Fonksiyon | Dönüş | Açıklama |
|---|---|---|
| `getBot()` | Bot | Panelde seçili bot (ya da scripte verilen bot). |
| `getBot(n)` | Bot / nil | Listede `n`. sıradaki bot (1'den başlar). |
| `getBot("isim")` | Bot / nil | GrowID'ye (büyük/küçük harf duyarsız), yoksa oyun içi isme göre. |
| `getBotById(id)` | Bot / nil | Sabit bot id'sine göre (`bot.id`). |
| `getBots()` | Bot dizisi | Bütün botlar, listedeki sırayla. |
| `addBot(growid, password, [proxy])` | Bot veya `nil, hata` | Yeni bot ekler (başlatmaz; `bot:connect()` çağır). |
| `removeBot(bot \| isim \| n)` | boolean | Botu durdurur ve listeden çıkarır. |
| `connectAll()` | – | Bağlı olmayan tüm botları `login_interval_ms` aralıklarla bağlar. |
| `disconnectAll()` | – | Tüm botların bağlantısını keser (offline'da bekler). |

Proxy formatı: `socks5://user:pass@host:port`, `socks5://host:port`, `host:port:user:pass` veya `host:port`.

### Olaylar

| Fonksiyon | Dönüş | Açıklama |
|---|---|---|
| `on(event, function(bot, ...) end)` | handler id | Tüm botlar için handler ekler. `event = "*"` her olayı alır: `function(bot, eventName, ...)`. |
| `off(id)` | silinen sayısı | Tek bir handler'ı siler. |
| `off(event)` | silinen sayısı | O olayın bütün handler'larını siler. |

### Script yönetimi

| Fonksiyon | Dönüş | Açıklama |
|---|---|---|
| `runScript(dosya, [bot])` | id veya `nil, hata` | `scripts/` içinden başka bir scripti ayrı thread'de başlatır. |
| `stopScript(id)` | – | Bir scripti durdurur. |
| `getScripts()` | dizi | `{ id, name, running, seconds }` tabloları. |

---

## 3. Bot nesnesi — özellikler

Hepsi salt-okunur, her okuma güncel değeri verir: `bot.world`, `bot.gems`...

| Özellik | Tip | Açıklama |
|---|---|---|
| `id` | integer | Sabit bot id'si (panelde `#`). `index` ile aynı. |
| `growid` | string | config'deki GrowID. |
| `name` | string | Oyun içi isim (renk kodsuz). |
| `status` | string | Durum anahtarı, bkz. [bölüm 6](#6-durum-status-değerleri). |
| `statusText` | string | Neden beklediği / durduğu ("server overloaded", "wrong GrowID / password"...). |
| `online` | boolean | Oyunda mı (menu, joining veya in_world). |
| `inWorld` | boolean | Bir world'ün içinde mi. |
| `running` | boolean | Bot thread'i çalışıyor mu (stopped değil). |
| `world` | string | Bulunduğu world (dışarıdaysa `""`). |
| `worldWidth`, `worldHeight` | integer | World boyutu (tile). |
| `targetWorld` | string | Kick / reconnect sonrası otomatik döneceği world. |
| `x`, `y` | number | Pozisyon (piksel). |
| `tileX`, `tileY` | integer | Pozisyon (tile; avatarın ortası, AquaProxy ile aynı hesap). |
| `gems` | integer | Gem sayısı. |
| `level` | integer | Seviye. |
| `ping` | integer | ms. |
| `netID`, `userID` | integer | Sunucunun verdiği id'ler. |
| `inventorySize` | integer | Envanter slot sayısı. |
| `proxy` | string | `host:port` (şifre gösterilmez) veya `""`. |
| `autoReconnect` | boolean | Kopunca otomatik bağlanır mı. |
| `walking` | boolean | Şu an bir `walk` yürüyor mu. |
| `autoCollect` | boolean | Yerdeki item'ları kendiliğinden topluyor mu. |

`tostring(bot)` → `Bot(#1 GROWID)`. İki bot nesnesi `==` ile karşılaştırılabilir.

---

## 4. Bot nesnesi — metotlar

Metotlar `:` ile çağrılır: `bot:warp("START")`.

### Bağlantı

| Metot | Açıklama |
|---|---|
| `bot:connect([gecikmeMs])` | Giriş yapar. Bot durmuşsa (stopped) veya offline ise yeniden başlatır. |
| `bot:disconnect()` | Bağlantıyı keser ve **offline** kalır (otomatik bağlanmaz). |
| `bot:reconnect()` | Bağlantıyı keser ve hemen yeniden giriş yapar. |
| `bot:setAutoReconnect(bool)` | Sunucu atınca otomatik bağlansın mı (varsayılan `true`). |

### Hareket ve world

| Metot | Açıklama |
|---|---|
| `bot:warp(world, [kapıId])` | World'e gider (tek seferlik). |
| `bot:warpAndWait(world, [timeoutMs=30000], [kapıId])` | Warp atar ve girene kadar bekler → `true/false`. |
| `bot:leave()` | World'den çıkar (world menüsüne). |
| `bot:setTargetWorld(world)` | Kalacağı world; atılınca / yeniden bağlanınca oraya döner. `""` = kapalı. |
| `bot:walk(tileX, tileY)` | O tile'a **yol bularak** yürür (A*), hemen döner. Yeni `walk` eskisinin yerini alır. |
| `bot:walkAndWait(tileX, tileY, [timeoutMs=30000])` | Yürür ve varana kadar bekler → `true` / `false, sebep`. |
| `bot:stopWalk()` | Yürümeyi durdurur. |
| `bot:findPath(tileX, tileY)` | Yürümeden yolu hesaplar → `{ {x, y}, ... }` (iki uç dahil) / `nil, sebep`. |
| `bot:move(dx, [dy=0])` | Bulunduğu tile'a göre göreli yürüme (`walk` ile aynı). |
| `bot:respawn()` | Respawn. |
| `bot:enter(tileX, tileY)` | O tile'daki kapıya / portala girer (5x5 içindeyse) → `boolean`. |

> **Pathfinding (AquaProxy'nin `/path`'i):** A* world'ün gerçek tile verisiyle çalışır: katı bloklar,
> kapalı kapılar, erişimimiz olmayan entrance'lar, VIP / friends entrance'ları, tek yönlü bloklar ve
> platformlar dikkate alınır; ateş / diken gibi zararlı tile'lardan mümkünse kaçınılır. Bot her pakette
> **bir tile** ilerler (iki adım arka arkaya, sonra 80–200 ms rastgele bekleme), adımlar reliable
> gönderilir (kaybolan adım sunucuda ışınlanma gibi görünürdü). Yol kapanırsa (kapı kapandı, blok
> koyuldu, sunucu botu başka yere koydu) bulunduğu yerden yeniden hesaplar (en fazla 25 kez).
> `items.dat` yoksa her ön plan bloğu katı sayılır, yani sadece boşluktan yürünür.

### Konuşma ve diyaloglar

| Metot | Açıklama |
|---|---|
| `bot:say(text)` / `bot:talk(text)` | Sohbete yazar. `/komut`lar da çalışır (`bot:say("/wave")`). |
| `bot:dialogReturn(isim, alanlar)` | `action|dialog_return` gönderir. `alanlar`: `{ key = value }` veya sıralı olması gerekiyorsa `{ {"key", value}, ... }`. |

### Envanter ve item

| Metot | Dönüş | Açıklama |
|---|---|---|
| `bot:getInventory()` | dizi | `{ id, name, count, flags }` tabloları. |
| `bot:getItemCount(id)` | integer | O itemden kaç tane var. |
| `bot:hasItem(id, [adet=1])` | boolean | |
| `bot:drop(id, [adet])` | boolean | Drop atar. `adet` yoksa hepsi. Yeterli item yoksa `false`. |
| `bot:trash(id, [adet])` | boolean | Çöpe atar. `adet` yoksa hepsi. |
| `bot:wear(id)` / `bot:unwear(id)` / `bot:use(id)` | – | Giy / çıkar / kullan (aynı paket: item activate). |

> Item isimleri `items.dat`'tan gelir: önce `GrowtopiaBot.exe`'nin yanındaki `items.dat`, yoksa oyunun
> önbelleği (`%LOCALAPPDATA%\Growtopia\cache\items.dat`). Yüklenmemişse isim yerine `"#id"` döner.

### World ile etkileşim

| Metot | Dönüş | Açıklama |
|---|---|---|
| `bot:punch(tileX, tileY)` | boolean | Yumruk atar. |
| `bot:place(tileX, tileY, itemId)` | boolean | Blok / tohum koyar (itemde yoksa yapmaz). |
| `bot:wrench(tileX, tileY)` | boolean | Wrench kullanır. |
| `bot:inReach(tileX, tileY)` | boolean | Tile botun 5x5 alanında mı. |

> ⚠️ **5x5 kuralı (ban koruması):** oyun yumruk / koyma / wrench / kapıya girmeyi sadece oyuncunun
> etrafındaki 5x5 kare içinde (her eksende en fazla 2 tile) yapar. mstate / smstate ile spawn olan
> karakter daha uzağa vurabilir, ama sunucu bu paketleri işaretler ve **banlar**. AquaProxy bunu uzak
> paketleri düşürerek engelliyor; bot da aynısını yapar: 5x5 dışındaki her tile işlemi (ham
> `sendPacket` dahil) **gönderilmez**, Log'a uyarı yazılır. Blok botun kendi tile'ına koyulamaz
> (background koyulabilir). Dönüş değeri çağrı anındaki bu kontroldür; uzaktaysan önce
> `walkAndWait` ile yaklaş.

### Tile verisi ve yerdeki item'lar

| Metot | Dönüş | Açıklama |
|---|---|---|
| `bot:getTile(tileX, tileY)` | tablo / nil | Tek tile (world dışıysa `nil`). |
| `bot:getTiles()` | dizi | World'deki bütün tile'lar (100x60 = 6000 tablo; sık çağırma). |
| `bot:findTiles(itemId)` | dizi | Ön planında veya arka planında o item olan tile'lar: `{ {x, y}, ... }`. |
| `bot:getObjects()` | dizi | Yerdeki (drop edilmiş) item'lar. |
| `bot:collect([yarıçap=2])` | integer | Yarıçap (tile, en fazla 2) içindeki item'ları toplar; menzildekilerin sayısını döner. Envantere sığmayanları (200'lük yığın, dolu çanta) almaz. |
| `bot:setAutoCollect(bool, [yarıçap=2])` | – | Yürürken, yeni drop düşünce ve 0.5 s'de bir kendiliğinden toplar. |

Tile tablosu: `{ x, y, fg, bg, fgName, bgName, flags, extraKind, walkable }`; kapı / tabelada `label`,
kilitte `owner` (userID), ağaçta `age` (saniye), `fruit` ve `ready` (büyüdü mü) de vardır.
Obje tablosu: `{ uid, id, name, count, x, y, tileX, tileY }`.

Tile'lar world girişindeki harita paketinden okunur ve sunucunun tile güncellemeleriyle (kırma,
koyma, tile update, ağaç hasadı) güncel tutulur.

### Item bilgisi (global)

| Fonksiyon | Dönüş | Açıklama |
|---|---|---|
| `getItemName(id)` | string | `"Dirt"`, yüklenmediyse `"#2"`. |
| `getItemId(isim)` | integer / nil | Büyük/küçük harf duyarsız: `getItemId("dirt seed")`. |
| `getItemInfo(id \| isim)` | tablo / nil | `{ id, name, type, collision, category, rarity, hits, growTime, background, info }`. |
| `itemsReady()` | boolean | `items.dat` yüklendi mi. |

### Oyuncular

| Metot | Dönüş | Açıklama |
|---|---|---|
| `bot:getPlayers()` | dizi | World'deki diğer oyuncular. |
| `bot:getPlayer(isim \| netID)` | tablo / nil | |

Oyuncu tablosu: `{ netID, userID, name, country, x, y, tileX, tileY, moderator }`.

### Durum sorgulama ve bekleme

Bekleyen metotlar beklerken olay handler'larını çalıştırır.

| Metot | Dönüş | Açıklama |
|---|---|---|
| `bot:getInfo()` | tablo | Bölüm 3'teki bütün özellikler tek tabloda. |
| `bot:isOnline()` | boolean | `bot.online` ile aynı. |
| `bot:isInWorld([world])` | boolean | Bir world'de (veya verilen world'de) mi. |
| `bot:waitForOnline([timeoutMs=60000])` | boolean | Giriş yapana kadar bekler. |
| `bot:waitForWorld([world], [timeoutMs=30000])` | boolean | Bir world'e (veya verilene) girene kadar. |
| `bot:waitForStatus(status, [timeoutMs=30000])` | boolean | `bot.status == status` olana kadar. |
| `bot:on(event, function(bot, ...) end)` | handler id | Sadece **bu botun** olayları. |
| `bot:wait(event, [timeoutMs])` | `bot, değerler...` / nil | Sadece bu botun olayını bekler. |

### Ham paketler (ileri seviye)

| Metot | Açıklama |
|---|---|
| `bot:sendText(text)` | `NET_MESSAGE_GENERIC_TEXT` (2). Örn. `"action|respawn\n"`. |
| `bot:sendGameMessage(text)` | `NET_MESSAGE_GAME_MESSAGE` (3). Örn. `"action|quit_to_exit\n"`. |
| `bot:sendPacket(tablo)` | Tank paketi (`NET_MESSAGE_GAME_PACKET`). |

`sendPacket` alanları (hepsi isteğe bağlı, varsayılan 0): `type, objectType, count1, count2,
netID, targetNetID, flags, floatValue, value, x, y, xSpeed, ySpeed, particle, punchX, punchY`,
ve `reliable` (varsayılan `true`).

```lua
-- yumruk animasyonuyla state paketi
bot:sendPacket({ type = 0, x = bot.x, y = bot.y, punchX = 10, punchY = 20, flags = 0x200 | 0x20, reliable = false })
```

---

## 5. Olaylar (events)

Handler'ın ilk argümanı **her zaman bot nesnesi**, ardından tablodaki değerler gelir:

```lua
on("console", function(bot, text, raw) print(bot.name, text) end)
local bot, world, w, h = wait("world_enter", 10000)
```

| Olay | Değerler (bot'tan sonra) | Ne zaman |
|---|---|---|
| `status` | `status, statusText` | Durum değişince. |
| `connected` | – | ENet bağlantısı kuruldu (giriş öncesi). |
| `login` | – | Giriş kabul edildi, oyuna giriyor. |
| `logon_fail` | `sebep` | Giriş reddedildi (2FA, overload, maintenance...). |
| `disconnected` | `sebep` | Bağlantı koptu. |
| `world_enter` | `world, genişlik, yükseklik` | World'e girdi. |
| `world_menu` | `çıkılanWorld` | World menüsüne döndü (çıktı / atıldı). |
| `join_fail` | `world` | World'e giremedi. |
| `spawn` | `netID, userID, x, y` | Bot world'de doğdu. |
| `player_join` | `netID, isim, userID, ülke, moderatörMü` | Biri world'e girdi. |
| `player_leave` | `netID, isim` | Biri world'den çıktı. |
| `moderator` | `isim, netID` | World'e moderatör/görünmez biri girdi. |
| `console` | `text, rawText` | Konsol mesajı (`text` renk kodsuz). |
| `talk` | `netID, isim, text` | Sohbet balonu (kendi mesajların dahil). |
| `dialog` | `dialogText` | Sunucu diyalog açtı (`OnDialogRequest`). |
| `inventory` | `itemSayısı, slotSayısı` | Envanterin tamamı geldi. |
| `item` | `itemId, değişim, yeniAdet` | Item kazanıldı (+) / kaybedildi (−). |
| `gems` | `gems` | Gem sayısı değişti. |
| `variant` | `fonksiyonAdı, arg1, arg2, ...` | Sunucudan gelen **her** `OnXxx` çağrısı (vec2/vec3 değerler `"x, y"` metni). |
| `packet` | `type, netID, value, x, y, punchX, punchY, flags` | Gelen her tank paketi. **Yoğun**: sadece gerekiyorsa dinle. |
| `text` | `text` | Ham `GENERIC_TEXT` mesajı. |
| `game_message` | `text` | Ham `GAME_MESSAGE` (`action|log` vb.). |
| `arrived` | `tileX, tileY` | `walk` hedefe vardı. |
| `path_fail` | `sebep, tileX, tileY` | `walk` bitti ama varamadı (yol yok, world değişti, durduruldu...). |
| `tile_change` | `tileX, tileY, fg, bg` | Bir tile değişti (kırıldı, koyuldu, güncellendi). |
| `object_drop` | `uid, itemId, adet, x, y` | Yere yeni bir item düştü. |
| `object_take` | `netID, itemId, adet` | Başka bir oyuncu yerden item aldı. |
| `collect` | `itemId, adet, uid` | Bot yerden item topladı. |
| `*` | `olayAdı, değerler...` | Hepsi (yoğun). |

Notlar:

- Bir olay **dinlenmiyorsa hiç üretilmez** (maliyeti ~0.5 ns). `packet`/`*` gibi yoğun olayları
  sadece gerektiğinde dinle, işin bitince `off(...)` yap.
- Her scriptin olay kuyruğu 20 000 olayla sınırlıdır; script hiç `sleep`/`wait` çağırmadan uzun
  süre meşgul kalırsa en eski olaylar düşer (panelde "dropped" sayısı görünür).
- Handler içinde `sleep`/`wait` kullanılabilir; o sırada gelen olaylar handler bitince işlenir.
- Handler hata verirse hata Output'a yazılır, script **devam eder**.

---

## 6. Durum (status) değerleri

| `bot.status` | Anlamı |
|---|---|
| `idle` | Henüz başlatılmadı. |
| `logging_in` | HTTP giriş (server_data / token). |
| `connecting` | Sunucuya bağlanıyor. |
| `entering_game` | Giriş kabul edildi, menüyü bekliyor. |
| `menu` | Oyunda, world seçme menüsünde. |
| `joining` | World'e giriyor. |
| `in_world` | Bir world'ün içinde. |
| `waiting` | Bekliyor (2FA, overload, maintenance, ağ hatası, login sırası). Sebep: `bot.statusText`. |
| `offline` | `disconnect()` ile kapatıldı; `connect()` bekliyor. |
| `stopped` | Durdu (yanlış şifre, deneme hakkı bitti, güncelleme gerekli, auto-reconnect kapalıyken kopma). `connect()` yeniden başlatır. |

---

## 7. Örnekler

### Bilgi okuma

```lua
local bot = getBot()
print(bot.name, bot.status, bot.world, bot.gems .. " gems", bot.ping .. " ms")
for _, item in ipairs(bot:getInventory()) do print(item.id, item.count) end
for _, p in ipairs(bot:getPlayers()) do print(p.name, p.tileX, p.tileY, p.moderator) end
```

### Bağlan, world'e git, konuş

```lua
local bot = getBot()
if not bot.online then
    bot:connect()
    assert(bot:waitForOnline(60000), "giriş olmadı: " .. bot.statusText)
end
if bot:warpAndWait("START") then bot:say("selam!") end
```

### Bütün botlar

```lua
for _, bot in ipairs(getBots()) do
    if not bot.online then bot:connect() sleep(3000) end
end
for _, bot in ipairs(getBots()) do
    if bot:waitForOnline(60000) then bot:setTargetWorld("MYWORLD") end
end
```

### Olaylara tepki

```lua
on("talk", function(bot, netID, name, text)
    if text == "!gems" then bot:say(name .. ", " .. bot.gems .. " gem var") end
end)
on("moderator", function(bot, name)
    warn("mod geldi: " .. name)
    bot:leave()
end)
on("disconnected", function(bot, reason) log(bot.name .. " koptu: " .. reason) end)
listen()
```

### Bir şeyi bekle

```lua
local bot = getBot()
bot:say("/time")
local b, text = bot:wait("console", 5000)
if b then print("sunucu cevabı:", text) else print("cevap gelmedi") end
```

### Belli miktarda item olunca drop

```lua
local bot = getBot()
while true do
    if bot.inWorld and bot:getItemCount(242) >= 50 then
        bot:warpAndWait("DEPOSIT")
        if bot:walkAndWait(10, 20) then
            bot:drop(242)          -- hepsini
        end
        sleep(1000)
    end
    sleep(5000)
end
```

### Yürü, kır, topla

```lua
local bot = getBot()
local ok, why = bot:walkAndWait(30, 20)
if not ok then error("varamadım: " .. why) end
local tile = bot:getTile(31, 20)
print(tile.fgName, tile.walkable)
while bot:getTile(31, 20).fg ~= 0 do
    bot:punch(31, 20)          -- 5x5 içinde
    sleep(250)
end
sleep(500)
print("topluyor:", bot:collect())
```

### Diyalog doldurma

```lua
bot:dialogReturn("drop_item", { {"itemID", "242|"}, {"count", 10} })
```

### Yeni bot ekleme

```lua
local bot, err = addBot("YENIBOT", "sifre", "socks5://user:pass@1.2.3.4:1080")
if not bot then error(err) end
bot:connect()   -- bot program kapanana kadar listede kalır; diske kaydedilmez
```

Daha fazlası: `Docs/examples/` (`example_basic.lua`, `example_events.lua`,
`example_multibot.lua`, `example_deposit.lua`).

---

## 8. Performans notları ve sınırlar

- Okuma maliyeti: `bot.gems` ≈ 0.1 µs, `bot:getInventory()` ≈ 0.25 µs, `bot:getPlayers()` ≈ 0.8 µs.
  Sıkı döngülerde bile rahat (ölçümler: `Docs/PERFORMANCE.md`).
- Komut kuyruğa alma (`say`, `drop`...) ≈ 0.3 µs; bot thread'i anında uyanır (~0.3 ms).
- **Oyunun kendisi** hızlı komutları sevmez: aynı anda çok sayıda warp/drop/say spam'i
  sunucudan kick/ban sebebi olabilir. Aralara `sleep` koy, girişleri `connectAll()` gibi aralıklı yap.
- Harita okuma ~70 µs, A* 7–50 µs (100x60 world, `--bench` bölüm 6); 100 bot world'deyken bile
  CPU değişmez.
- 5x5 dışına yumruk / koyma / wrench / kapı ve 2 tile'dan uzaktaki item'ı toplama **hiç gönderilmez**.
- Script içinden `io`/`os` gibi standart kütüphaneler açık (dosya okuma/yazma yapılabilir) — sadece
  güvendiğin scriptleri çalıştır.
