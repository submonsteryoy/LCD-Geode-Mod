#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr auto API_URL = "https://api-lcd.vercel.app/levels";

    std::string stringField(matjson::Value const& item, std::string_view key) {
        if (!item.isObject() || !item.contains(key)) return "";
        return item[key].asString().unwrapOr("");
    }

    int intField(matjson::Value const& item, std::string_view key) {
        if (!item.isObject() || !item.contains(key)) return 0;
        return item[key].asInt().map([](std::intmax_t value) {
            return static_cast<int>(value);
        }).unwrapOr(0);
    }
}

class $modify(LCDMenuLayer, MenuLayer) {
    struct Fields {
        async::TaskHolder<web::WebResponse> m_request;
    };

    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        auto menu = this->getChildByID("bottom-menu");
        if (!menu) {
            log::warn("LCD: bottom-menu was not found");
            return true;
        }

        auto button = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("LCD"),
            this,
            menu_selector(LCDMenuLayer::onLCD)
        );

        button->setID("lcd-button"_spr);
        menu->addChild(button);
        menu->updateLayout();

        return true;
    }

    void onLCD(CCObject*) {
        auto loading = FLAlertLayer::create(
            "LCD Community List",
            "Loading the live list...",
            "OK"
        );
        loading->show();

        auto request = web::WebRequest();
        request.timeout(std::chrono::seconds(15));
        request.userAgent("LCD-Geode-Mod/1.0");

        m_fields->m_request.spawn(
            request.get(API_URL),
            [](web::WebResponse response) {
                if (!response.ok()) {
                    FLAlertLayer::create(
                        "LCD Community List",
                        fmt::format("API request failed (HTTP {}).", response.code()),
                        "OK"
                    )->show();
                    return;
                }

                auto jsonResult = response.json();
                if (jsonResult.isErr()) {
                    FLAlertLayer::create(
                        "LCD Community List",
                        "The API responded, but the JSON could not be parsed.",
                        "OK"
                    )->show();
                    return;
                }

                auto json = jsonResult.unwrap();
                if (!json.isObject() || !json.contains("items")) {
                    FLAlertLayer::create(
                        "LCD Community List",
                        "The API response does not contain an items array.",
                        "OK"
                    )->show();
                    return;
                }

                auto itemsResult = json["items"].asArray();
                if (itemsResult.isErr()) {
                    FLAlertLayer::create(
                        "LCD Community List",
                        "The API items field is not an array.",
                        "OK"
                    )->show();
                    return;
                }

                std::vector<std::pair<int, std::string>> rows;
                for (auto const& item : itemsResult.unwrap()) {
                    auto title = stringField(item, "title");
                    if (title.empty()) title = "Untitled";
                    rows.emplace_back(intField(item, "rank"), std::move(title));
                }

                std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) {
                    if (a.first == 0) return false;
                    if (b.first == 0) return true;
                    return a.first < b.first;
                });

                std::string text = fmt::format(
                    "<cy>Live LCD list loaded!</c>\n<cg>{} levels</c>\n\n",
                    rows.size()
                );

                auto shown = std::min<std::size_t>(rows.size(), 12);
                for (std::size_t i = 0; i < shown; ++i) {
                    if (rows[i].first > 0) {
                        text += fmt::format("#{}  {}\n", rows[i].first, rows[i].second);
                    } else {
                        text += fmt::format("{}\n", rows[i].second);
                    }
                }

                if (rows.size() > shown) {
                    text += fmt::format("\n...and {} more.", rows.size() - shown);
                }

                FLAlertLayer::create(
                    "LCD Community List",
                    text,
                    "OK"
                )->show();
            }
        );
    }
};
