#include "Items.hpp"
#include "../Core/Log.hpp"
#include "../Core/Text.hpp"
#include "../Main/Config.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ItemDB {
    namespace {
        /* @important: the highest items.dat layout this parser knows. A newer
           file is still read (see the anchor resync below), and the report
           says so. */
        constexpr uint8_t MAX_VERSION = 24;

        /* How far to look for a lost id anchor. Newer versions have appended a
           byte or two at a time; the window is far too small to "find" an
           anchor by coincidence. */
        constexpr uint32_t WIDE_SKEW = 50000;

        constexpr std::string_view NAME_KEY{ "PBG892FXX982ABC*" };

        std::vector<Item> gItems;
        std::unordered_map<std::string, size_t> gByName; /* lower-case name -> index */
        std::atomic<bool> gReady{ false };
        std::atomic<bool> gStarted{ false };
        std::mutex gStatusMutex;
        std::string gStatus = "not loaded";

        void SetStatus(std::string text) {
            std::lock_guard lock(gStatusMutex);
            gStatus = std::move(text);
        }

        /* Bounds-checked cursor. Any read past the end flips `ok` and yields
           zeros, so a corrupt or unknown file fails cleanly instead of reading
           outside the buffer. */
        class Reader {
        public:
            explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}

            size_t pos = 0;
            bool ok = true;

            template <typename T>
            T Read() {
                T value{};
                if (!Need(sizeof(T))) return value;
                std::memcpy(&value, data_.data() + pos, sizeof(T));
                pos += sizeof(T);
                return value;
            }

            void Skip(size_t count) {
                if (Need(count)) pos += count;
            }

            void SkipString() { Skip(Read<uint16_t>()); }

            std::string ReadString() {
                const uint16_t length = Read<uint16_t>();
                if (!Need(length)) return {};
                std::string value(reinterpret_cast<const char*>(data_.data() + pos), length);
                pos += length;
                return value;
            }

            bool Fits(size_t at, size_t count) const { return at <= data_.size() && data_.size() - at >= count; }

            uint32_t U32At(size_t at) const {
                uint32_t value = 0;
                if (Fits(at, 4)) std::memcpy(&value, data_.data() + at, 4);
                return value;
            }

        private:
            bool Need(size_t count) {
                if (ok && Fits(pos, count)) return true;
                ok = false;
                return false;
            }

            const std::vector<uint8_t>& data_;
        };

        std::vector<std::filesystem::path> Candidates() {
            std::vector<std::filesystem::path> paths{ Config::ExeDirectory() / "items.dat" };
            char* local = nullptr;
            size_t length = 0;
            if (_dupenv_s(&local, &length, "LOCALAPPDATA") == 0 && local) {
                paths.push_back(std::filesystem::path(local) / "Growtopia" / "cache" / "items.dat");
                std::free(local);
            }
            return paths;
        }

        /* Item records are stored in id order and each begins with its id as a
           32-bit little-endian value. That is a checkable anchor: a version
           that only APPENDS fields can be followed without knowing what it
           added -- see how far the anchor moved and carry that as padding.
           A field inserted mid-record moves the anchor unpredictably, the
           resync fails, and we stop: no names beats wrong names. */
        bool Resync(Reader& reader, uint32_t id, int64_t& extraPerRecord) {
            const size_t pos = reader.pos;
            for (uint32_t skip = 1; skip <= WIDE_SKEW; ++skip) {
                if (!reader.Fits(pos + skip, 4)) break;
                if (reader.U32At(pos + skip) == id) {
                    extraPerRecord += skip;
                    reader.pos = pos + skip;
                    return true;
                }
            }
            /* The parser may also have overshot. */
            for (uint32_t skip = 1; skip <= WIDE_SKEW && skip <= pos; ++skip) {
                if (reader.U32At(pos - skip) == id) {
                    extraPerRecord -= skip;
                    reader.pos = pos - skip;
                    return true;
                }
            }
            return false;
        }

        bool Parse(const std::vector<uint8_t>& data, std::vector<Item>& items, std::string& report) {
            Reader reader(data);
            const uint8_t version = reader.Read<uint8_t>(); reader.Skip(1);
            const uint16_t count = reader.Read<uint16_t>(); reader.Skip(2);

            std::string note;
            if (version > MAX_VERSION) {
                note = " [v" + std::to_string(version) + " is newer than the v" + std::to_string(MAX_VERSION) + " layout this parser knows]";
            }

            int64_t extraPerRecord = 0;
            items.reserve(count);

            for (uint16_t i = 0; i < count; ++i) {
                if (!reader.ok || !reader.Fits(reader.pos, 4)) {
                    report = "parse ran off the end after " + std::to_string(items.size()) + " of " + std::to_string(count) +
                        " items (v" + std::to_string(version) + ")";
                    return false;
                }

                if (reader.U32At(reader.pos) != i && !Resync(reader, i, extraPerRecord)) {
                    report = "v" + std::to_string(version) + " changed in a way this parser cannot follow (lost the id anchor at item " +
                        std::to_string(items.size()) + " of " + std::to_string(count) + ")";
                    return false;
                }

                Item item;
                item.id = reader.Read<uint16_t>(); reader.Skip(2);
                item.property = reader.Read<uint8_t>();
                item.category = reader.Read<uint8_t>();
                item.type = reader.Read<uint8_t>();
                reader.Skip(1);

                const uint16_t nameLength = reader.Read<uint16_t>();
                if (reader.Fits(reader.pos, nameLength)) {
                    item.name.resize(nameLength);
                    for (uint16_t c = 0; c < nameLength; ++c) {
                        item.name[c] = static_cast<char>(data[reader.pos + c] ^ NAME_KEY[(c + item.id) % NAME_KEY.size()]);
                    }
                }
                reader.Skip(nameLength);

                reader.SkipString();        /* texture */
                reader.Skip(4 + 1);         /* texture hash, visual effect */
                item.ingredient = reader.Read<int>();
                reader.Skip(4);             /* texture x/y, spread type, stripey wallpaper */

                item.collision = reader.Read<uint8_t>();
                item.hits = reader.Read<uint8_t>();
                if (item.hits != 0) item.hits /= 6; /* stored as hits * 6 */
                item.hitReset = reader.Read<int>();

                if (item.type == Type::CLOTHING) item.clothType = reader.Read<uint8_t>();
                else reader.Skip(1);
                item.rarity = reader.Read<int16_t>();

                reader.Skip(1);             /* max amount */
                reader.SkipString();        /* extra file (audio) */
                reader.Skip(4 + 4);         /* extra file hash, audio volume */
                for (int s = 0; s < 4; ++s) reader.SkipString(); /* pet name, prefix, suffix, ability */
                reader.Skip(16);

                item.growTime = reader.Read<int>();
                reader.Skip(2 + 2);
                for (int s = 0; s < 3; ++s) reader.SkipString();
                reader.Skip(80);

                if (version >= 11) reader.SkipString();                       // @date February 2019
                if (version >= 12) reader.Skip(4 + 9);                        // @date October 2020
                if (version >= 13) reader.Skip(4);                            // @date May 2021
                if (version >= 14) reader.Skip(4);                            // @date October 2021
                if (version >= 15) { reader.Skip(25); reader.SkipString(); }
                if (version >= 16) reader.SkipString();
                if (version >= 17) reader.Skip(4);                            // @date April 2024
                if (version >= 18) reader.Skip(4);                            // @date December 2024
                if (version >= 19) reader.Skip(9);
                if (version >= 21) reader.Skip(2);                            // @date September 2025
                if (version >= 22) item.info = reader.ReadString();
                if (version >= 23) {
                    item.splice[0] = reader.Read<uint16_t>();
                    item.splice[1] = reader.Read<uint16_t>();
                }
                if (version >= 24) reader.Skip(1);                            // @date December 2025

                /* Whatever a newer version appended, learned from the file. */
                if (extraPerRecord < 0 && reader.pos < static_cast<size_t>(-extraPerRecord)) reader.ok = false;
                else reader.pos = static_cast<size_t>(static_cast<int64_t>(reader.pos) + extraPerRecord);

                if (!reader.ok) {
                    report = "record " + std::to_string(i) + " is truncated (v" + std::to_string(version) + ")";
                    return false;
                }
                items.push_back(std::move(item));
            }

            report = std::to_string(items.size()) + " items, v" + std::to_string(version) +
                (extraPerRecord ? " (" + std::string(extraPerRecord > 0 ? "+" : "") + std::to_string(extraPerRecord) + " byte(s)/record learned from the file)" : "") +
                note;
            return true;
        }

        void Load() {
            std::string tried;
            for (const auto& path : Candidates()) {
                std::error_code error;
                const auto size = std::filesystem::file_size(path, error);
                if (error || size == 0) {
                    tried += (tried.empty() ? "" : ", ") + path.string();
                    continue;
                }

                std::vector<uint8_t> data(static_cast<size_t>(size));
                std::ifstream file(path, std::ios::binary);
                if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
                    Log::Error("ITEMS", "couldn't read {}", path.string());
                    tried += (tried.empty() ? "" : ", ") + path.string();
                    continue;
                }

                std::vector<Item> items;
                std::string report;
                if (!Parse(data, items, report)) {
                    Log::Error("ITEMS", "{}: {} -- item names and tile collisions are unavailable", path.string(), report);
                    SetStatus("failed: " + report);
                    return;
                }

                gItems = std::move(items);
                for (size_t i = 0; i < gItems.size(); ++i) gByName.try_emplace(Text::Lower(gItems[i].name), i);
                SetStatus(report + " from " + path.string());
                gReady.store(true, std::memory_order_release);
                Log::Info("ITEMS", "loaded {} from {}", report, path.string());
                return;
            }
            Log::Warn("ITEMS", "items.dat not found (looked at {}). Copy it next to GrowtopiaBot.exe for item names; "
                "the pathfinder treats every block as solid meanwhile", tried);
            SetStatus("not found -- put items.dat next to GrowtopiaBot.exe");
        }
    }

    void LoadAsync() {
        if (gStarted.exchange(true)) return;
        SetStatus("loading...");
        std::thread([] {
            try { Load(); }
            catch (const std::exception& e) {
                Log::Error("ITEMS", "items.dat load failed: {}", e.what());
                SetStatus(std::string("failed: ") + e.what());
            }
        }).detach();
    }

    bool Ready() noexcept { return gReady.load(std::memory_order_acquire); }

    std::string Status() {
        std::lock_guard lock(gStatusMutex);
        return gStatus;
    }

    size_t Count() noexcept { return Ready() ? gItems.size() : 0; }

    const Item* Find(int id) noexcept {
        if (!Ready() || id < 0) return nullptr;
        /* Records are stored in id order, so the id is normally the index. */
        if (static_cast<size_t>(id) < gItems.size() && gItems[static_cast<size_t>(id)].id == id) return &gItems[static_cast<size_t>(id)];
        for (const Item& item : gItems) {
            if (item.id == id) return &item;
        }
        return nullptr;
    }

    const Item* Find(std::string_view name) {
        if (!Ready()) return nullptr;
        const auto it = gByName.find(Text::Lower(Text::Trim(name)));
        return it != gByName.end() ? &gItems[it->second] : nullptr;
    }

    std::string Name(int id) {
        const Item* item = Find(id);
        return item && !item->name.empty() ? item->name : "#" + std::to_string(id);
    }

    bool IsBackground(int id) noexcept {
        /* Backgrounds go behind the player, so the game lets them be placed on our own tile. */
        const Item* item = Find(id);
        return item && (item->type == Type::BACKGROUND || item->type == Type::SFX_BACKGROUND || item->type == Type::ART_WALL);
    }
}
