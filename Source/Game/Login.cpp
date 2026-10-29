#include "Login.hpp"
#include "../Core/Crypto.hpp"
#include "../Core/Json.hpp"
#include "../Core/Text.hpp"

#include <format>

namespace Login {
    namespace {
        constexpr const char* UBI_AGENT = "UbiServices_SDK_2022.Release.9_PC64_ansi_static";
        constexpr const char* BROWSER_AGENT = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)";
        constexpr const char* VAL_KEY = "40db4045f2d8c572efe8c4a060605726";
        constexpr const char* VALIDATE_URL = "https://login.growtopiagame.com/player/growid/login/validate";

        std::string_view EnclosingTag(std::string_view html, size_t position) {
            const size_t open = html.rfind('<', position);
            const size_t close = html.find('>', position);
            if (open == std::string_view::npos || close == std::string_view::npos) return {};
            return html.substr(open, close - open + 1);
        }

        std::string Attribute(std::string_view tag, std::string_view name) {
            for (const char quote : { '"', '\'' }) {
                const std::string needle = std::string(name) + "=" + quote;
                size_t at = tag.find(needle);
                while (at != std::string_view::npos && at > 0 && !std::isspace(static_cast<unsigned char>(tag[at - 1]))) {
                    at = tag.find(needle, at + 1);
                }
                if (at == std::string_view::npos) continue;
                const size_t begin = at + needle.size();
                const size_t end = tag.find(quote, begin);
                if (end == std::string_view::npos) continue;
                return Text::HtmlDecode(tag.substr(begin, end - begin));
            }
            return {};
        }

        std::string CsrfToken(std::string_view html) {
            for (const std::string_view marker : { std::string_view("name=\"_token\""), std::string_view("name='_token'") }) {
                const size_t at = html.find(marker);
                if (at == std::string_view::npos) continue;
                const std::string value = Attribute(EnclosingTag(html, at), "value");
                if (!value.empty()) return value;
            }
            return {};
        }

        std::string DangerText(std::string_view html) {
            const size_t at = html.find("text-danger-wrapper");
            if (at == std::string_view::npos) return {};
            const size_t start = html.find('>', at);
            if (start == std::string_view::npos) return {};
            std::string text;
            int depth = 0;
            for (size_t i = start + 1; i < html.size(); ++i) {
                if (html[i] == '<') {
                    if (html.substr(i, 2) == "</") {
                        if (depth == 0) break;
                        --depth;
                    }
                    else ++depth;
                    const size_t close = html.find('>', i);
                    if (close == std::string_view::npos) break;
                    i = close;
                    continue;
                }
                text += html[i];
            }
            return std::string(Text::Trim(Text::HtmlDecode(text)));
        }

        std::string PipeBody(std::initializer_list<std::pair<std::string_view, std::string>> fields) {
            std::string body;
            for (const auto& [key, value] : fields) {
                body += key;
                body += '|';
                body += value;
                body += '\n';
            }
            return body;
        }

        std::string FormBody(std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
            std::string body;
            for (const auto& [key, value] : fields) {
                if (!body.empty()) body += '&';
                body += Text::UrlEncode(key);
                body += '=';
                body += Text::UrlEncode(value);
            }
            return body;
        }
    }

    std::optional<ServerData> FetchServerData(Net::HttpClient& http, const ClientInfo& client, bool alternate, std::string& error) {
        const std::string url = alternate ? "https://www.growtopia2.com/growtopia/server_data.php"
                                          : "https://www.growtopia1.com/growtopia/server_data.php";
        const std::string body = std::format("platform=0&protocol={}&version={}", client.protocol, client.gameVersion);
        const auto response = http.Post(url, { { "User-Agent", UBI_AGENT } }, body, error, false);
        if (!response) return std::nullopt;
        if (response->status != 200) {
            error = std::format("server_data.php answered HTTP {}", response->status);
            return std::nullopt;
        }

        ServerData data;
        for (std::string_view line : Text::Split(response->body, '\n')) {
            if (line.starts_with("RTENDMARKERBS1001")) break;
            const size_t bar = line.find('|');
            if (bar == std::string_view::npos) continue;
            const std::string_view key = Text::Trim(line.substr(0, bar));
            const std::string value(Text::Trim(line.substr(bar + 1)));
            if (key == "server") data.server = value;
            else if (key == "port") data.port = static_cast<uint16_t>(Text::ToInt(value).value_or(0));
            else if (key == "loginurl") data.loginUrl = value;
            else if (key == "meta") data.meta = value;
            else if (key == "#maint") data.maintenance = value;
        }
        if (data.server.empty() || data.port == 0) {
            error = "server_data.php had no server/port" + (data.maintenance.empty() ? std::string() : " (maintenance: " + data.maintenance + ")");
            return std::nullopt;
        }
        return data;
    }

    Result LegacyToken(Net::HttpClient& http, const ClientInfo& client, const ServerData& server,
        const std::string& growId, const std::string& password, std::string& token) {
        std::string error;

        const std::string rid = Crypto::GenerateRid();
        const int32_t hash = Crypto::HashString("RT");
        const std::string protocol = std::to_string(client.protocol);
        const std::string dashboardBody = PipeBody({
            { "tankIDName", "" }, { "tankIDPass", "" }, { "requestedName", "" }, { "f", "1" },
            { "protocol", protocol }, { "game_version", client.gameVersion }, { "cbits", "0" }, { "player_age", "25" },
            { "GDPR", "1" }, { "FCMToken", "" }, { "category", "_-5100" }, { "totalPlaytime", "0" },
            { "klv", Crypto::Klv(client.gameVersion, protocol, rid, hash) }, { "meta", server.meta },
            { "fhash", std::to_string(client.fhash) }, { "rid", rid }, { "platformID", "2" }, { "deviceVersion", "0" },
            { "country", "us" }, { "hash", std::to_string(hash) }, { "mac", "02:00:00:00:00:00" }, { "wk", "NONE0" },
        });
        const auto dashboard = http.Post(std::format("https://{}/player/login/dashboard?valKey={}", server.loginUrl, VAL_KEY),
            { { "User-Agent", BROWSER_AGENT } }, dashboardBody, error);
        if (!dashboard) return { Error::Network, "dashboard: " + error };

        if (Text::Trim(dashboard->body).starts_with('{')) {
            std::string message = dashboard->body;
            try { message = Json::Parse(dashboard->body)["message"].String(message); }
            catch (...) {}
            return { Error::Network, "dashboard returned an error: " + message };
        }

        std::string growtopiaUrl;
        if (const size_t at = dashboard->body.find("optionChose('Grow')"); at != std::string::npos) {
            growtopiaUrl = Attribute(EnclosingTag(dashboard->body, at), "href");
        }
        if (growtopiaUrl.empty()) return { Error::Network, std::format("dashboard (HTTP {}) has no GrowID login link", dashboard->status) };

        const auto page = http.Get(growtopiaUrl, { { "User-Agent", BROWSER_AGENT } }, error);
        if (!page) return { Error::Network, "login page: " + error };
        const std::string csrf = CsrfToken(page->body);
        if (csrf.empty()) return { Error::Network, std::format("login page (HTTP {}) has no CSRF token", page->status) };

        const auto validate = http.Post(VALIDATE_URL, {
                { "User-Agent", BROWSER_AGENT },
                { "Origin", "https://login.growtopiagame.com" },
                { "Referer", growtopiaUrl },
            },
            FormBody({ { "_token", csrf }, { "growId", growId }, { "password", password } }), error);
        if (!validate) return { Error::Network, "validate: " + error };

        if (Text::Trim(validate->body).starts_with('{')) {
            try {
                const Json json = Json::Parse(validate->body);
                token = json["token"].String();
                if (!token.empty()) return {};
                return { Error::Other, "login response has no token: " + validate->body.substr(0, 200) };
            }
            catch (const std::exception& e) {
                return { Error::Other, std::string("login response: ") + e.what() };
            }
        }

        const std::string danger = DangerText(validate->body);
        if (!danger.empty()) {
            if (Text::IContains(danger, "exhausted")) return { Error::Exhausted, danger };
            if (Text::IContains(danger, "mismatched")) return { Error::WrongCredentials, danger };
            return { Error::Other, danger };
        }
        return { Error::Other, std::format("login failed: unexpected response (HTTP {})", validate->status) };
    }

    Result CheckToken(Net::HttpClient& http, const std::string& token, const std::string& clientData, std::string& newToken) {
        if (token.empty()) return { Error::Other, "token is empty" };
        std::string error;
        const auto response = http.Post(std::format("https://login.growtopiagame.com/player/growid/checktoken?valKey={}", VAL_KEY),
            { { "User-Agent", UBI_AGENT } }, FormBody({ { "refreshToken", token }, { "clientData", clientData } }), error);
        if (!response) return { Error::Network, "checktoken: " + error };

        try {
            const Json json = Json::Parse(response->body);
            if (json["status"].String() == "success") {
                newToken = json["token"].String();
                return {};
            }
            return { Error::Other, "checktoken refused: " + response->body.substr(0, 200) };
        }
        catch (const std::exception& e) {
            return { Error::Other, std::string("checktoken: ") + e.what() };
        }
    }
}
