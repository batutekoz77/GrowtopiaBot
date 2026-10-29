# Performans

GrowtopiaBot'un içinde kendi test araçları var. Hiçbiri gerçek Growtopia sunucusuna ya da hesaba
dokunmaz: bot'lar, `127.0.0.1` üzerinde çalışan **sahte bir ENet sunucusuna** bağlanır. Bu sunucu
login → world → trafik akışını taklit eder ve kendi CPU'su ölçümden düşülür.

```
GrowtopiaBot.exe --bench [bot=50] [saniye=10] [bot başına paket/s=20]   -> bench_result.txt
GrowtopiaBot.exe --bench-ui [bot=100] [saniye=8]                        -> bench_ui_result.txt (pencere açılır)
GrowtopiaBot.exe --selftest                                              -> selftest_result.txt (Lua API testleri)
```

CPU, `QueryProcessCycleTime` / `QueryThreadCycleTime` ile ölçülür (çevrim sayacı).
`GetProcessTimes` 15.6 ms'lik tick'lerle saydığı için kısa aralıklarla çalışan thread'lerde
(event-driven bot, boşta bekleyen panel) yanlış sonuç veriyor.

## Sonuçlar (12 çekirdek, 100 bot, bot başına 20 paket/s world trafiği)

### Bot ağ döngüsü

| | Önce (5 ms sabit poll) | Sonra (wake socket + 50 ms) | Fark |
|---|---|---|---|
| CPU (100 bot) | 61.3 % (bir çekirdeğin) | 14.5 % | **−76 %** |
| Bot başına uyanma | 205 /s | 31 /s | **−85 %** |
| Komut gecikmesi (ortalama) | 2.76 ms | 0.36 ms | **7.7× daha hızlı** |
| Komut gecikmesi (p99) | 37.8 ms | 2.28 ms | |
| İşlenen paket | 2207 /s | 2192 /s | aynı |

Makine boşken yapılan diğer koşularda oran −72 % ile −82 % arasındaydı. Makine yüklüyken mutlak
değerler değişiyor, ama önce/sonra oranı her koşuda korundu.

**Ne değişti:**
- Eskiden her bot thread'i `select()` ile **5 ms'de bir** uyanıyordu (bot başına 200/s), bir şey
  olsa da olmasa da. Artık thread ENet / proxy soketlerinde ve bir **wake socket**'te
  (127.0.0.1 UDP) uyuyor. Paket gelince ya da panelden / scriptten komut gelince (`Post()`)
  **anında** uyanıyor. Zaman aşımı (50 ms) sadece ENet'in kendi zamanlayıcıları için
  (resend / ping); bunlar zaten ≥50 ms adımlarla çalışıyor.
- Offline / bekleyen botlar 50 ms yerine 250 ms'de bir uyanıyor (komut gelirse yine anında).
- Ping değeri sadece değiştiğinde yazılıyor (eskiden her döngüde kilit alınıyordu).
- `Log::Debug` debug kapalıyken metni hiç formatlamıyor.
- Her bot eskiden ayarlardaki **bütün bot listesinin** bir kopyasını tutuyordu (N bot →
  N² kayıt, şifreler dahil). Artık sadece ortak ayarları tutuyor (`Config::Common`).
- Konsol çıktısı (Windows konsolu çok yavaştır) varsayılan olarak kapalı. Loglar panelde bir
  ring buffer'da tutuluyor: 4 thread'den saniyede ~2.5 milyon satır.

**Düzeltilen hatalar (test sırasında bulundu):**
- *Kaçan uyandırma:* wake bayrağı soket boşaltılmadan **önce** temizlenince, araya giren bir
  komut 50 ms (offline'da 250 ms) bekleyebiliyordu. `--bench` içindeki doğruluk testi eski
  sırayla 6000 komutta **272 kaçırma** buldu, düzeltmeden sonra **0**.
- *OnSpawn pozisyonu:* `posXY` geçici bir string'in `string_view`'ıyla parse ediliyordu (dangling).
  Botun kendi X pozisyonu rastgele bozulabiliyordu. `--selftest` yakaladı, düzeltildi.

### World verisi ve pathfinding (`--bench` bölüm 6)

Sahte sunucunun 100x60 haritası (duvarlı), 2000 tekrarın ortalaması:

| İşlem | Süre |
|---|---|
| Harita paketini okuma (48 KB, 6000 tile) | ~71 µs |
| A* duvarın etrafından (25 tile) | ~7 µs |
| A* world boyunca (108 tile) | ~11 µs |
| A* hedefe yol yok (bütün açık alanı tarar) | ~52 µs |

Her bot kendi world'ünü tutar; tile başına 24 byte (ek veri sadece kapı, tabela, kilit, ağaç gibi
tile'larda ayrıca ayrılır), yani 100x60 bir world ≈ 144 KB. 100 bot world'deyken ağ döngüsünün
CPU'su değişmedi: bu makinede eski loop 11.3 %, yeni loop **5.0 %** (bir çekirdeğin), p99 komut
gecikmesi 3.1 ms.

**Bulunan hata:** yürüme adımları başta (Mori gibi) unreliable gönderiliyordu. `--selftest`'teki sahte
sunucu her state paketinde pozisyon farkını kontrol ediyor ve 2–3 tile'lık **ışınlanmalar** yakaladı:
ENet'in throttle'ı unreliable paketleri düşürebiliyor, kaybolan adım sunucuda ışınlanma gibi görünüyor.
Adımlar artık reliable; 6 koşuda 0 ışınlanma, 0 menzil (5x5) ihlali.

### Lua API (tek script, world içindeki bot)

| İşlem | Maliyet |
|---|---|
| `bot.gems`, `bot.world` (özellik okuma) | 0.10 – 0.25 µs |
| `bot:getItemCount(id)` | 0.13 – 0.36 µs |
| `bot:getInventory()` | 0.25 – 0.53 µs |
| `bot:getPlayers()` | 0.8 – 1.7 µs |
| `bot:sendText(...)` (komut kuyruğa) | 0.3 – 0.8 µs |
| Olay üretimi (`Emit`) | 150 – 360 ns |
| Lua'da işlenen olay | 300 000 – 385 000 /s |
| Dinlenmeyen olay kontrolü | ~0.5 ns |

Aralıklar, makine boşken ve yüklüyken yapılan koşulardan geliyor. Her script kendi thread'inde
çalışıyor; yoğun bir script bot thread'lerini beklemiyor.

### Panel (300 canlı bot)

| Durum | FPS | CPU (bir çekirdeğin) |
|---|---|---|
| Kullanırken (her kare çizilir) | 60–144 | ~3 % (kare başı 0.12 ms UI + 0.09 ms DirectX) |
| Boşta / arka planda | ~9–30 | ~1 % |
| Simge durumunda / tamamen kapalı | 0 | ~0 % |

Açılış maliyeti bir kerelik: DirectX + font atlası, 0.4–1 s. Bot verisi her karede değil,
saniyede 10 kez okunuyor. Uzun listeler (bot tablosu, log, script çıktısı) `ImGuiListClipper`
ile sadece ekranda görünen satırları çiziyor.

### Script satır izleme (editördeki renkler)

Sadece editörden (Run / F5) çalıştırılan **tek** script izlenir; diğer bütün scriptler (
`runScript`, "Run for every bot"daki diğer botlar) satır hook'u olmadan çalışır, maliyetleri sıfır.
İzlenen scriptte her yeni satır bir hook çağrısı demek:

| Saf Lua döngüsü (3 satır / tur) | tur/s |
|---|---|
| İzleme kapalı | ~67 milyon |
| İzleme açık | ~9 milyon (satır başına ~30 ns) |

Bot API'si / `sleep` ağırlıklı normal scriptlerde fark hissedilmez.
