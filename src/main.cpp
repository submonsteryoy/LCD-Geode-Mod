#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/LoadingSpinner.hpp>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr auto API_URL = "https://api-lcd.vercel.app/levels";

    struct RankItemData {
        int rank = 0;
        int levelId = 0;
        std::string title;
        std::string creator;
        std::string difficulty;
        std::string description;
        std::vector<std::string> beatenBy;
        std::vector<std::string> youtubeUrls;
    };

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

    int flexibleIntField(matjson::Value const& item, std::string_view key) {
        if (!item.isObject() || !item.contains(key)) return 0;

        auto asInt = item[key].asInt();
        if (!asInt.isErr()) {
            return static_cast<int>(asInt.unwrap());
        }

        auto asString = item[key].asString();
        if (!asString.isErr()) {
            try {
                return std::stoi(asString.unwrap());
            } catch (...) {
                return 0;
            }
        }

        return 0;
    }

    std::vector<std::string> stringArrayField(
        matjson::Value const& item,
        std::string_view key
    ) {
        std::vector<std::string> values;

        if (!item.isObject() || !item.contains(key)) {
            return values;
        }

        auto arrayResult = item[key].asArray();
        if (arrayResult.isErr()) {
            return values;
        }

        for (auto const& value : arrayResult.unwrap()) {
            auto text = value.asString();
            if (!text.isErr()) {
                values.push_back(text.unwrap());
            }
        }

        return values;
    }

    std::string join(std::vector<std::string> const& values, std::string const& separator) {
        std::string result;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) result += separator;
            result += values[i];
        }
        return result;
    }
}

class LCDListPopup : public geode::Popup {
protected:
    async::TaskHolder<web::WebResponse> m_request;

    ScrollLayer* m_scroll = nullptr;
    LoadingSpinner* m_spinner = nullptr;
    CCLabelBMFont* m_status = nullptr;

    std::vector<RankItemData> m_items;

    static constexpr float POPUP_W = 440.f;
    static constexpr float POPUP_H = 270.f;
    static constexpr float LIST_W = 390.f;
    static constexpr float LIST_H = 190.f;
    static constexpr float ROW_H = 42.f;
    static constexpr float ROW_GAP = 4.f;

    bool init() {
        if (!Popup::init(POPUP_W, POPUP_H)) {
            return false;
        }

        this->setTitle("LCD Community List");

        auto size = m_mainLayer->getContentSize();

        // Small subtitle / status line.
        m_status = CCLabelBMFont::create("Connecting to LCD...", "goldFont.fnt");
        m_status->setScale(.42f);
        m_status->setPosition({size.width / 2.f, size.height - 42.f});
        m_mainLayer->addChild(m_status);

        // Refresh button.
        auto refreshSprite = ButtonSprite::create("Refresh");
        refreshSprite->setScale(.55f);

        auto refreshButton = CCMenuItemSpriteExtra::create(
            refreshSprite,
            this,
            menu_selector(LCDListPopup::onRefresh)
        );

        auto topMenu = CCMenu::create();
        topMenu->setPosition({size.width - 53.f, size.height - 42.f});
        topMenu->addChild(refreshButton);
        m_mainLayer->addChild(topMenu);

        // Scrollable list area.
        m_scroll = ScrollLayer::create(
            CCSize{LIST_W, LIST_H},
            true,
            true
        );
        m_scroll->setPosition({
            (size.width - LIST_W) / 2.f,
            18.f
        });
        m_scroll->setStealingTouches(true);
        m_mainLayer->addChild(m_scroll);

        // Loading spinner.
        m_spinner = LoadingSpinner::create(32.f);
        m_spinner->setPosition({
            size.width / 2.f,
            18.f + LIST_H / 2.f
        });
        m_mainLayer->addChild(m_spinner, 20);

        this->fetchLevels();
        return true;
    }

    void setStatus(std::string const& text) {
        if (m_status) {
            m_status->setString(text.c_str());
        }
    }

    void setLoading(bool loading) {
        if (m_spinner) {
            m_spinner->setVisible(loading);
        }
        if (m_scroll) {
            m_scroll->setVisible(!loading);
        }
    }

    void fetchLevels() {
        this->setLoading(true);
        this->setStatus("Loading live list...");

        auto request = web::WebRequest();
        request.timeout(std::chrono::seconds(15));
        request.userAgent("LCD-Geode-Mod/1.1");

        m_request.spawn(
            "Fetch LCD Community List",
            request.get(API_URL),
            [this](web::WebResponse response) {
                if (!response.ok()) {
                    this->setLoading(false);
                    this->setStatus(fmt::format("API error: HTTP {}", response.code()));

                    FLAlertLayer::create(
                        "LCD Community List",
                        fmt::format(
                            "Couldn't load the list.\nHTTP {}",
                            response.code()
                        ),
                        "OK"
                    )->show();
                    return;
                }

                auto jsonResult = response.json();
                if (jsonResult.isErr()) {
                    this->setLoading(false);
                    this->setStatus("Invalid API response");

                    FLAlertLayer::create(
                        "LCD Community List",
                        "The LCD API responded, but the JSON could not be read.",
                        "OK"
                    )->show();
                    return;
                }

                auto json = jsonResult.unwrap();
                if (!json.isObject() || !json.contains("items")) {
                    this->setLoading(false);
                    this->setStatus("No items returned");
                    return;
                }

                auto itemsResult = json["items"].asArray();
                if (itemsResult.isErr()) {
                    this->setLoading(false);
                    this->setStatus("Invalid items array");
                    return;
                }

                std::vector<RankItemData> parsed;

                for (auto const& item : itemsResult.unwrap()) {
                    RankItemData data;
                    data.rank = intField(item, "rank");
                    // This field is optional for now. The current API returns full
                    // RankItem objects, so once you add `level_id` in Base44,
                    // it will begin appearing here automatically.
                    data.levelId = flexibleIntField(item, "level_id");
                    data.title = stringField(item, "title");
                    data.creator = stringField(item, "creator");
                    data.difficulty = stringField(item, "difficulty");
                    data.description = stringField(item, "description");
                    data.beatenBy = stringArrayField(item, "beaten_by");
                    data.youtubeUrls = stringArrayField(item, "youtube_urls");

                    if (data.title.empty()) {
                        data.title = "Untitled";
                    }

                    parsed.push_back(std::move(data));
                }

                std::sort(
                    parsed.begin(),
                    parsed.end(),
                    [](RankItemData const& a, RankItemData const& b) {
                        if (a.rank == 0 && b.rank == 0) return a.title < b.title;
                        if (a.rank == 0) return false;
                        if (b.rank == 0) return true;
                        return a.rank < b.rank;
                    }
                );

                m_items = std::move(parsed);
                this->rebuildList();
                this->setLoading(false);
                this->setStatus(fmt::format("{} levels - live", m_items.size()));
            }
        );
    }

    CCNode* createRowVisual(RankItemData const& item, std::size_t index) {
        auto row = CCLayerColor::create(
            ccc4(0, 0, 0, index % 2 == 0 ? 75 : 45),
            LIST_W - 8.f,
            ROW_H
        );

        // Rank.
        auto rankLabel = CCLabelBMFont::create(
            item.rank > 0 ? fmt::format("#{}", item.rank).c_str() : "-",
            "bigFont.fnt"
        );
        rankLabel->setScale(.55f);
        rankLabel->setAnchorPoint({0.f, .5f});
        rankLabel->setPosition({9.f, ROW_H / 2.f});
        row->addChild(rankLabel);

        // Level title.
        auto titleLabel = CCLabelBMFont::create(
            item.title.c_str(),
            "bigFont.fnt"
        );
        titleLabel->setScale(.52f);
        titleLabel->setAnchorPoint({0.f, .5f});
        titleLabel->setPosition({58.f, ROW_H / 2.f + 7.f});
        titleLabel->limitLabelWidth(210.f, .52f, .25f);
        row->addChild(titleLabel);

        // Creator + difficulty.
        auto creatorText = item.creator.empty() ? "Unknown creator" : item.creator;
        auto info = fmt::format(
            "{}  |  {}",
            creatorText,
            item.difficulty.empty() ? "Demon" : item.difficulty
        );

        auto infoLabel = CCLabelBMFont::create(
            info.c_str(),
            "goldFont.fnt"
        );
        infoLabel->setScale(.32f);
        infoLabel->setAnchorPoint({0.f, .5f});
        infoLabel->setPosition({58.f, ROW_H / 2.f - 9.f});
        infoLabel->limitLabelWidth(270.f, .32f, .18f);
        row->addChild(infoLabel);

        // Arrow means the row is tappable. Once level_id exists, tapping the
        // row opens that exact ID in Geometry Dash.
        auto arrow = CCLabelBMFont::create(">", "bigFont.fnt");
        arrow->setScale(.55f);
        arrow->setPosition({LIST_W - 29.f, ROW_H / 2.f});
        row->addChild(arrow);

        return row;
    }

    void rebuildList() {
        if (!m_scroll) return;

        auto content = m_scroll->m_contentLayer;
        content->removeAllChildrenWithCleanup(true);

        if (m_items.empty()) {
            auto empty = CCLabelBMFont::create("No levels found.", "bigFont.fnt");
            empty->setScale(.6f);
            empty->setPosition({LIST_W / 2.f, LIST_H / 2.f});
            content->addChild(empty);
            content->setContentSize({LIST_W, LIST_H});
            return;
        }

        float totalHeight =
            static_cast<float>(m_items.size()) * (ROW_H + ROW_GAP) + ROW_GAP;

        totalHeight = std::max(totalHeight, LIST_H);
        content->setContentSize({LIST_W, totalHeight});

        for (std::size_t i = 0; i < m_items.size(); ++i) {
            float y =
                totalHeight
                - ROW_GAP
                - ROW_H / 2.f
                - static_cast<float>(i) * (ROW_H + ROW_GAP);

            auto visual = this->createRowVisual(m_items[i], i);

            auto button = CCMenuItemSpriteExtra::create(
                visual,
                this,
                menu_selector(LCDListPopup::onLevel)
            );
            button->setTag(static_cast<int>(i));

            auto menu = CCMenu::create();
            menu->setContentSize({LIST_W, ROW_H});
            menu->setPosition({LIST_W / 2.f, y});
            menu->addChild(button);

            content->addChild(menu);
        }

        m_scroll->scrollToTop();
    }

    void onRefresh(CCObject*) {
        this->fetchLevels();
    }

    void onLevel(CCObject* sender) {
        auto button = static_cast<CCMenuItemSpriteExtra*>(sender);
        auto index = static_cast<std::size_t>(button->getTag());

        if (index >= m_items.size()) {
            return;
        }

        auto const& item = m_items[index];

        // ID-ready behavior:
        // If `level_id` exists in Base44, take the player to a GD search for
        // that exact level ID. Searching an exact numeric ID yields the target
        // level in Geometry Dash's normal level browser.
        if (item.levelId > 0) {
            auto query = fmt::format("{}", item.levelId);
            auto search = GJSearchObject::create(SearchType::Search, query);

            if (search) {
                auto scene = LevelBrowserLayer::scene(search);
                CCDirector::sharedDirector()->replaceScene(
                    CCTransitionFade::create(.35f, scene)
                );
            }
            return;
        }

        // Until you add level_id to the website, the UI still works normally
        // and shows the information already available from RankItem.
        std::string details;

        if (item.rank > 0) {
            details += fmt::format("<cy>Rank:</c> #{}\n", item.rank);
        }

        if (!item.creator.empty()) {
            details += fmt::format("<cy>Creator:</c> {}\n", item.creator);
        }

        if (!item.difficulty.empty()) {
            details += fmt::format("<cy>Difficulty:</c> {}\n", item.difficulty);
        }

        if (!item.beatenBy.empty()) {
            details += fmt::format(
                "<cy>Beaten by:</c> {}\n",
                join(item.beatenBy, ", ")
            );
        }

        if (!item.description.empty()) {
            details += fmt::format("\n{}", item.description);
        }

        if (!item.youtubeUrls.empty()) {
            details += fmt::format(
                "\n\n<cg>{} completion video{}</c>",
                item.youtubeUrls.size(),
                item.youtubeUrls.size() == 1 ? "" : "s"
            );
        }

        details +=
            "\n\n<cy>GD link:</c> Level ID has not been added to this entry yet.";

        FLAlertLayer::create(
            item.title.c_str(),
            details,
            "OK"
        )->show();
    }

public:
    static LCDListPopup* create() {
        auto ret = new LCDListPopup();

        if (ret->init()) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }
};

class $modify(LCDMenuLayer, MenuLayer) {
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
        auto popup = LCDListPopup::create();
        if (popup) {
            popup->show();
        }
    }
};
