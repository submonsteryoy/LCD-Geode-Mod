#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/LoadingSpinner.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
        std::string createdDate;
        std::vector<std::string> beatenBy;
        std::vector<std::string> youtubeUrls;
    };

    std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

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

    std::string stableKey(RankItemData const& item) {
        if (item.levelId > 0) {
            return fmt::format("id:{}", item.levelId);
        }
        return "name:" + lower(item.title) + "|creator:" + lower(item.creator);
    }

    std::string progressKey(RankItemData const& item) {
        return "progress::" + stableKey(item);
    }

    std::string completedKey(RankItemData const& item) {
        return "completed::" + stableKey(item);
    }

    std::string favoriteKey(RankItemData const& item) {
        return "favorite::" + stableKey(item);
    }

    int getProgress(RankItemData const& item) {
        return Mod::get()->getSavedValue<int>(progressKey(item), 0);
    }

    bool isCompleted(RankItemData const& item) {
        return Mod::get()->getSavedValue<bool>(completedKey(item), false)
            || getProgress(item) >= 100;
    }

    bool isFavorite(RankItemData const& item) {
        return Mod::get()->getSavedValue<bool>(favoriteKey(item), false);
    }

    void setProgress(RankItemData const& item, int value) {
        value = std::clamp(value, 0, 100);
        Mod::get()->setSavedValue<int>(progressKey(item), value);
        if (value >= 100) {
            Mod::get()->setSavedValue<bool>(completedKey(item), true);
        }
    }

    std::vector<RankItemData> parseItems(matjson::Value const& json) {
        std::vector<RankItemData> parsed;

        if (!json.isObject() || !json.contains("items")) {
            return parsed;
        }

        auto itemsResult = json["items"].asArray();
        if (itemsResult.isErr()) {
            return parsed;
        }

        for (auto const& item : itemsResult.unwrap()) {
            RankItemData data;
            data.rank = intField(item, "rank");
            data.levelId = flexibleIntField(item, "level_id");
            data.title = stringField(item, "title");
            data.creator = stringField(item, "creator");
            data.difficulty = stringField(item, "difficulty");
            data.description = stringField(item, "description");
            data.createdDate = stringField(item, "created_date");
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

        return parsed;
    }

    void openGDLevel(RankItemData const& item) {
        if (item.levelId <= 0) {
            FLAlertLayer::create(
                "Level ID Missing",
                "This entry does not have a <cy>level_id</c> yet. "
                "Once the website adds one, this same mod can open it directly.",
                "OK"
            )->show();
            return;
        }

        auto search = GJSearchObject::create(
            SearchType::Search,
            fmt::format("{}", item.levelId)
        );

        if (!search) {
            FLAlertLayer::create(
                "LCD",
                "Geometry Dash could not create the level search.",
                "OK"
            )->show();
            return;
        }

        auto scene = LevelBrowserLayer::scene(search);
        CCDirector::sharedDirector()->replaceScene(
            CCTransitionFade::create(.35f, scene)
        );
    }
}

class LCDLevelPopup : public geode::Popup {
protected:
    RankItemData m_item;
    std::function<void()> m_onChanged;

    TextInput* m_progressInput = nullptr;
    CCLabelBMFont* m_localStatus = nullptr;
    CCMenuItemSpriteExtra* m_favoriteButton = nullptr;
    CCMenuItemSpriteExtra* m_completedButton = nullptr;

    bool init(RankItemData item, std::function<void()> onChanged) {
        if (!Popup::init(405.f, 260.f)) {
            return false;
        }

        m_item = std::move(item);
        m_onChanged = std::move(onChanged);

        this->setTitle(m_item.title);

        auto size = m_mainLayer->getContentSize();

        // Core details
        std::string info;
        if (m_item.rank > 0) info += fmt::format("<cy>Rank:</c> #{}\n", m_item.rank);
        if (!m_item.creator.empty()) info += fmt::format("<cy>Creator:</c> {}\n", m_item.creator);
        if (!m_item.difficulty.empty()) info += fmt::format("<cy>Difficulty:</c> {}\n", m_item.difficulty);

        if (!m_item.beatenBy.empty()) {
            auto beaten = join(m_item.beatenBy, ", ");
            if (beaten.size() > 115) beaten = beaten.substr(0, 112) + "...";
            info += fmt::format("<cy>Beaten by:</c> {}\n", beaten);
        }

        if (!m_item.description.empty()) {
            auto desc = m_item.description;
            if (desc.size() > 250) desc = desc.substr(0, 247) + "...";
            info += "\n" + desc;
        }

        auto infoText = TextArea::create(
            info.empty() ? "No extra information yet." : info,
            "chatFont.fnt",
            0.45f,
            330.f,
            {0.5f, 1.f},
            16.f,
            false
        );
        infoText->setPosition({size.width / 2.f, size.height - 51.f});
        m_mainLayer->addChild(infoText);

        // Local progress input
        auto progressLabel = CCLabelBMFont::create("Personal best %", "goldFont.fnt");
        progressLabel->setScale(.38f);
        progressLabel->setPosition({70.f, 66.f});
        m_mainLayer->addChild(progressLabel);

        m_progressInput = TextInput::create(72.f, "0-100", "bigFont.fnt");
        m_progressInput->setFilter("0123456789");
        m_progressInput->setMaxCharCount(3);
        m_progressInput->setString(fmt::format("{}", getProgress(m_item)), false);
        m_progressInput->setPosition({70.f, 43.f});
        m_mainLayer->addChild(m_progressInput);

        m_localStatus = CCLabelBMFont::create("", "goldFont.fnt");
        m_localStatus->setScale(.34f);
        m_localStatus->setPosition({size.width / 2.f, 18.f});
        m_mainLayer->addChild(m_localStatus);

        // Main action menu
        auto actionMenu = CCMenu::create();
        actionMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(actionMenu);

        auto play = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(m_item.levelId > 0 ? "Play Level" : "No ID Yet"),
            this,
            menu_selector(LCDLevelPopup::onPlay)
        );
        play->setScale(.67f);
        play->setPosition({195.f, 77.f});
        actionMenu->addChild(play);

        auto video = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(m_item.youtubeUrls.empty() ? "No Video" : "Video"),
            this,
            menu_selector(LCDLevelPopup::onVideo)
        );
        video->setScale(.58f);
        video->setPosition({307.f, 77.f});
        actionMenu->addChild(video);

        auto save = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Save %"),
            this,
            menu_selector(LCDLevelPopup::onSaveProgress)
        );
        save->setScale(.55f);
        save->setPosition({145.f, 43.f});
        actionMenu->addChild(save);

        auto fav = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(isFavorite(m_item) ? "★ Favorite" : "☆ Favorite"),
            this,
            menu_selector(LCDLevelPopup::onFavorite)
        );
        fav->setScale(.55f);
        fav->setPosition({245.f, 43.f});
        actionMenu->addChild(fav);
        m_favoriteButton = fav;

        auto done = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(isCompleted(m_item) ? "✓ Completed" : "Mark Done"),
            this,
            menu_selector(LCDLevelPopup::onCompleted)
        );
        done->setScale(.52f);
        done->setPosition({337.f, 43.f});
        actionMenu->addChild(done);
        m_completedButton = done;

        this->refreshLocalStatus();
        return true;
    }

    void refreshLocalStatus() {
        auto progress = getProgress(m_item);
        auto text = fmt::format(
            "{}%  •  {}  •  {}",
            progress,
            isCompleted(m_item) ? "Completed" : "In progress",
            isFavorite(m_item) ? "Favorite" : "Not favorited"
        );
        m_localStatus->setString(text.c_str());
    }

    void notifyChanged() {
        this->refreshLocalStatus();
        if (m_onChanged) m_onChanged();
    }

    void onPlay(CCObject*) {
        openGDLevel(m_item);
    }

    void onVideo(CCObject*) {
        if (m_item.youtubeUrls.empty()) {
            FLAlertLayer::create(
                "LCD",
                "No completion video is attached to this entry.",
                "OK"
            )->show();
            return;
        }

        geode::utils::web::openLinkInBrowser(m_item.youtubeUrls.front());
    }

    void onSaveProgress(CCObject*) {
        int value = 0;
        try {
            value = std::stoi(std::string(m_progressInput->getString().c_str()));
        } catch (...) {
            value = 0;
        }

        value = std::clamp(value, 0, 100);
        m_progressInput->setString(fmt::format("{}", value), false);
        setProgress(m_item, value);

        if (value < 100 && Mod::get()->getSavedValue<bool>(completedKey(m_item), false)) {
            // Keep manually-completed entries completed even if best is edited downward.
        }

        this->notifyChanged();
    }

    void onFavorite(CCObject*) {
        auto next = !isFavorite(m_item);
        Mod::get()->setSavedValue<bool>(favoriteKey(m_item), next);

        // Recreate button text visually by changing its child ButtonSprite string if available.
        auto buttonSprite = typeinfo_cast<ButtonSprite*>(m_favoriteButton->getNormalImage());
        if (buttonSprite) {
            buttonSprite->setString(next ? "★ Favorite" : "☆ Favorite");
        }

        this->notifyChanged();
    }

    void onCompleted(CCObject*) {
        auto next = !isCompleted(m_item);
        Mod::get()->setSavedValue<bool>(completedKey(m_item), next);

        if (next && getProgress(m_item) < 100) {
            setProgress(m_item, 100);
            m_progressInput->setString("100", false);
        }

        auto buttonSprite = typeinfo_cast<ButtonSprite*>(m_completedButton->getNormalImage());
        if (buttonSprite) {
            buttonSprite->setString(next ? "✓ Completed" : "Mark Done");
        }

        this->notifyChanged();
    }

public:
    static LCDLevelPopup* create(
        RankItemData item,
        std::function<void()> onChanged = {}
    ) {
        auto ret = new LCDLevelPopup();
        if (ret->init(std::move(item), std::move(onChanged))) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }
};

class LCDListPopup : public geode::Popup {
protected:
    enum class Tab {
        All = 0,
        Top = 1,
        New = 2,
        Favorites = 3,
        Completed = 4
    };

    async::TaskHolder<web::WebResponse> m_request;

    ScrollLayer* m_scroll = nullptr;
    LoadingSpinner* m_spinner = nullptr;
    TextInput* m_searchInput = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_filterStatus = nullptr;

    std::vector<RankItemData> m_items;
    std::vector<std::size_t> m_filteredIndexes;

    Tab m_tab = Tab::All;
    std::string m_query;
    int m_difficultyIndex = 0;
    bool m_usingCache = false;

    std::vector<std::string> m_difficulties {
        "All",
        "Extreme",
        "Insane",
        "Hard",
        "Medium",
        "Easy"
    };

    static constexpr float POPUP_W = 455.f;
    static constexpr float POPUP_H = 285.f;
    static constexpr float LIST_W = 410.f;
    static constexpr float LIST_H = 155.f;
    static constexpr float ROW_H = 40.f;
    static constexpr float ROW_GAP = 3.f;

    bool init() {
        if (!Popup::init(POPUP_W, POPUP_H)) {
            return false;
        }

        this->setTitle("LCD Community List");

        auto size = m_mainLayer->getContentSize();

        // Search
        m_searchInput = TextInput::create(185.f, "Search level or creator...", "bigFont.fnt");
        m_searchInput->setMaxCharCount(40);
        m_searchInput->setPosition({115.f, size.height - 43.f});
        m_searchInput->setCallback([this](std::string const& value) {
            m_query = lower(value);
            this->rebuildList();
        });
        m_mainLayer->addChild(m_searchInput);

        // Top controls
        auto topMenu = CCMenu::create();
        topMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(topMenu);

        auto refresh = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Refresh"),
            this,
            menu_selector(LCDListPopup::onRefresh)
        );
        refresh->setScale(.53f);
        refresh->setPosition({252.f, size.height - 43.f});
        topMenu->addChild(refresh);

        auto random = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Random"),
            this,
            menu_selector(LCDListPopup::onRandom)
        );
        random->setScale(.53f);
        random->setPosition({325.f, size.height - 43.f});
        topMenu->addChild(random);

        auto diff = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Diff"),
            this,
            menu_selector(LCDListPopup::onDifficulty)
        );
        diff->setScale(.53f);
        diff->setPosition({391.f, size.height - 43.f});
        topMenu->addChild(diff);

        m_filterStatus = CCLabelBMFont::create("Difficulty: All", "goldFont.fnt");
        m_filterStatus->setScale(.30f);
        m_filterStatus->setAnchorPoint({1.f, .5f});
        m_filterStatus->setPosition({432.f, size.height - 65.f});
        m_mainLayer->addChild(m_filterStatus);

        // Tabs
        auto tabMenu = CCMenu::create();
        tabMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(tabMenu);

        struct TabSpec {
            char const* text;
            Tab tab;
            float x;
        };

        std::vector<TabSpec> tabs {
            {"ALL", Tab::All, 62.f},
            {"TOP", Tab::Top, 138.f},
            {"NEW", Tab::New, 214.f},
            {"FAV", Tab::Favorites, 290.f},
            {"DONE", Tab::Completed, 370.f}
        };

        for (auto const& spec : tabs) {
            auto button = CCMenuItemSpriteExtra::create(
                ButtonSprite::create(spec.text),
                this,
                menu_selector(LCDListPopup::onTab)
            );
            button->setScale(.50f);
            button->setTag(static_cast<int>(spec.tab));
            button->setPosition({spec.x, size.height - 83.f});
            tabMenu->addChild(button);
        }

        m_status = CCLabelBMFont::create("Connecting to LCD...", "goldFont.fnt");
        m_status->setScale(.32f);
        m_status->setAnchorPoint({0.f, .5f});
        m_status->setPosition({23.f, 17.f});
        m_mainLayer->addChild(m_status);

        m_scroll = ScrollLayer::create(
            CCSize{LIST_W, LIST_H},
            true,
            true
        );
        m_scroll->setPosition({
            (size.width - LIST_W) / 2.f,
            30.f
        });
        m_scroll->setStealingTouches(true);
        m_mainLayer->addChild(m_scroll);

        m_spinner = LoadingSpinner::create(32.f);
        m_spinner->setPosition({
            size.width / 2.f,
            30.f + LIST_H / 2.f
        });
        m_mainLayer->addChild(m_spinner, 20);

        this->fetchLevels();
        return true;
    }

    std::unordered_set<std::string> recentNewSet() const {
        auto list = Mod::get()->getSavedValue<std::vector<std::string>>(
            "recent-new-keys",
            {}
        );
        return std::unordered_set<std::string>(list.begin(), list.end());
    }

    bool isNew(RankItemData const& item) const {
        auto keys = this->recentNewSet();
        return keys.contains(stableKey(item));
    }

    std::vector<std::string> makeSnapshot(std::vector<RankItemData> const& items) const {
        std::vector<std::string> out;
        out.reserve(items.size());

        for (auto const& item : items) {
            out.push_back(fmt::format(
                "{}\x1e{}",
                stableKey(item),
                item.rank
            ));
        }

        return out;
    }

    std::unordered_map<std::string, int> snapshotMap(
        std::vector<std::string> const& snapshot
    ) const {
        std::unordered_map<std::string, int> out;

        for (auto const& line : snapshot) {
            auto pos = line.rfind('\x1e');
            if (pos == std::string::npos) continue;

            auto key = line.substr(0, pos);
            int rank = 0;

            try {
                rank = std::stoi(line.substr(pos + 1));
            } catch (...) {
                rank = 0;
            }

            out[key] = rank;
        }

        return out;
    }

    void detectChanges(std::vector<RankItemData> const& freshItems) {
        auto previous = Mod::get()->getSavedValue<std::vector<std::string>>(
            "list-snapshot",
            {}
        );

        auto current = this->makeSnapshot(freshItems);
        auto oldMap = this->snapshotMap(previous);
        auto newMap = this->snapshotMap(current);

        std::vector<std::string> addedKeys;
        int moved = 0;
        int removed = 0;

        if (!oldMap.empty()) {
            for (auto const& [key, rank] : newMap) {
                auto it = oldMap.find(key);
                if (it == oldMap.end()) {
                    addedKeys.push_back(key);
                } else if (it->second != rank) {
                    ++moved;
                }
            }

            for (auto const& [key, rank] : oldMap) {
                if (!newMap.contains(key)) {
                    ++removed;
                }
            }
        }

        Mod::get()->setSavedValue("list-snapshot", current);
        Mod::get()->setSavedValue("recent-new-keys", addedKeys);

        if (!oldMap.empty() && (!addedKeys.empty() || moved > 0 || removed > 0)) {
            FLAlertLayer::create(
                "LCD List Updated!",
                fmt::format(
                    "<cg>{} new</c>  •  <cy>{} moved</c>  •  <cr>{} removed</c>",
                    addedKeys.size(),
                    moved,
                    removed
                ),
                "Nice"
            )->show();
        }
    }

    void setLoading(bool loading) {
        if (m_spinner) m_spinner->setVisible(loading);
        if (m_scroll) m_scroll->setVisible(!loading);
    }

    void setStatus(std::string const& text) {
        if (m_status) m_status->setString(text.c_str());
    }

    bool applyJson(matjson::Value const& json, bool fromCache) {
        auto parsed = parseItems(json);
        if (parsed.empty()) {
            return false;
        }

        m_items = std::move(parsed);
        m_usingCache = fromCache;

        if (!fromCache) {
            this->detectChanges(m_items);
            Mod::get()->setSavedValue<std::string>("cache-json", json.dump());
        }

        this->setLoading(false);
        this->rebuildList();

        if (fromCache) {
            this->setStatus(fmt::format(
                "OFFLINE CACHE  •  {} levels",
                m_items.size()
            ));
        } else {
            this->setStatus(fmt::format(
                "LIVE  •  {} levels  •  synced now",
                m_items.size()
            ));
        }

        return true;
    }

    bool loadCache() {
        auto cached = Mod::get()->getSavedValue<std::string>("cache-json", "");
        if (cached.empty()) {
            return false;
        }

        auto parsed = matjson::Value::parse(cached);
        if (parsed.isErr()) {
            return false;
        }

        return this->applyJson(parsed.unwrap(), true);
    }

    void fetchLevels() {
        this->setLoading(true);
        this->setStatus("Syncing LCD...");

        auto request = web::WebRequest();
        request.timeout(std::chrono::seconds(15));
        request.userAgent("LCD-Geode-Mod/1.4");

        m_request.spawn(
            "Fetch LCD Community List",
            request.get(API_URL),
            [this](web::WebResponse response) {
                if (!response.ok()) {
                    if (this->loadCache()) {
                        return;
                    }

                    this->setLoading(false);
                    this->setStatus(fmt::format("API error: HTTP {}", response.code()));

                    FLAlertLayer::create(
                        "LCD Community List",
                        "The live API could not be reached and no offline cache exists yet.",
                        "OK"
                    )->show();
                    return;
                }

                auto jsonResult = response.json();
                if (jsonResult.isErr() || !this->applyJson(jsonResult.unwrap(), false)) {
                    if (this->loadCache()) {
                        return;
                    }

                    this->setLoading(false);
                    this->setStatus("Invalid API response");

                    FLAlertLayer::create(
                        "LCD Community List",
                        "The API responded, but the level list could not be read.",
                        "OK"
                    )->show();
                }
            }
        );
    }

    bool passesDifficulty(RankItemData const& item) const {
        if (m_difficultyIndex == 0) return true;

        auto needle = lower(m_difficulties[m_difficultyIndex]);
        auto haystack = lower(item.difficulty);

        return haystack.find(needle) != std::string::npos;
    }

    bool passesQuery(RankItemData const& item) const {
        if (m_query.empty()) return true;

        auto title = lower(item.title);
        auto creator = lower(item.creator);
        auto difficulty = lower(item.difficulty);

        return title.find(m_query) != std::string::npos
            || creator.find(m_query) != std::string::npos
            || difficulty.find(m_query) != std::string::npos;
    }

    bool passesTab(RankItemData const& item) const {
        switch (m_tab) {
            case Tab::All:
                return true;

            case Tab::Top:
                return item.rank > 0 && item.rank <= 10;

            case Tab::New:
                return this->isNew(item);

            case Tab::Favorites:
                return isFavorite(item);

            case Tab::Completed:
                return isCompleted(item);
        }

        return true;
    }

    void updateFilteredIndexes() {
        m_filteredIndexes.clear();

        for (std::size_t i = 0; i < m_items.size(); ++i) {
            auto const& item = m_items[i];

            if (
                this->passesQuery(item)
                && this->passesDifficulty(item)
                && this->passesTab(item)
            ) {
                m_filteredIndexes.push_back(i);
            }
        }
    }

    CCNode* createRowVisual(RankItemData const& item, std::size_t visibleIndex) {
        auto row = CCLayerColor::create(
            ccc4(0, 0, 0, visibleIndex % 2 == 0 ? 78 : 48),
            LIST_W - 8.f,
            ROW_H
        );

        auto rankLabel = CCLabelBMFont::create(
            item.rank > 0 ? fmt::format("#{}", item.rank).c_str() : "-",
            "bigFont.fnt"
        );
        rankLabel->setScale(.52f);
        rankLabel->setAnchorPoint({0.f, .5f});
        rankLabel->setPosition({8.f, ROW_H / 2.f});
        row->addChild(rankLabel);

        auto titlePrefix = isFavorite(item) ? "★ " : "";
        auto title = titlePrefix + item.title;

        auto titleLabel = CCLabelBMFont::create(
            title.c_str(),
            "bigFont.fnt"
        );
        titleLabel->setScale(.49f);
        titleLabel->setAnchorPoint({0.f, .5f});
        titleLabel->setPosition({57.f, ROW_H / 2.f + 7.f});
        titleLabel->limitLabelWidth(205.f, .49f, .24f);
        row->addChild(titleLabel);

        auto info = fmt::format(
            "{}  •  {}",
            item.creator.empty() ? "Unknown creator" : item.creator,
            item.difficulty.empty() ? "Unknown difficulty" : item.difficulty
        );

        auto infoLabel = CCLabelBMFont::create(info.c_str(), "goldFont.fnt");
        infoLabel->setScale(.29f);
        infoLabel->setAnchorPoint({0.f, .5f});
        infoLabel->setPosition({57.f, ROW_H / 2.f - 8.f});
        infoLabel->limitLabelWidth(230.f, .29f, .17f);
        row->addChild(infoLabel);

        auto progress = getProgress(item);
        std::string progressText;
        if (isCompleted(item)) {
            progressText = "✓ 100%";
        } else if (progress > 0) {
            progressText = fmt::format("{}%", progress);
        } else {
            progressText = "--";
        }

        auto progressLabel = CCLabelBMFont::create(
            progressText.c_str(),
            isCompleted(item) ? "goldFont.fnt" : "bigFont.fnt"
        );
        progressLabel->setScale(.36f);
        progressLabel->setPosition({333.f, ROW_H / 2.f});
        row->addChild(progressLabel);

        if (this->isNew(item)) {
            auto newLabel = CCLabelBMFont::create("NEW", "goldFont.fnt");
            newLabel->setScale(.28f);
            newLabel->setPosition({375.f, ROW_H / 2.f + 8.f});
            row->addChild(newLabel);
        }

        auto arrow = CCLabelBMFont::create(">", "bigFont.fnt");
        arrow->setScale(.48f);
        arrow->setPosition({LIST_W - 24.f, ROW_H / 2.f});
        row->addChild(arrow);

        return row;
    }

    void rebuildList() {
        if (!m_scroll) return;

        this->updateFilteredIndexes();

        auto content = m_scroll->m_contentLayer;
        content->removeAllChildrenWithCleanup(true);

        if (m_filteredIndexes.empty()) {
            auto empty = CCLabelBMFont::create(
                "No matching LCD levels.",
                "bigFont.fnt"
            );
            empty->setScale(.48f);
            empty->setPosition({LIST_W / 2.f, LIST_H / 2.f});
            content->addChild(empty);
            content->setContentSize({LIST_W, LIST_H});
            return;
        }

        float totalHeight =
            static_cast<float>(m_filteredIndexes.size()) * (ROW_H + ROW_GAP) + ROW_GAP;

        totalHeight = std::max(totalHeight, LIST_H);
        content->setContentSize({LIST_W, totalHeight});

        for (std::size_t visibleIndex = 0; visibleIndex < m_filteredIndexes.size(); ++visibleIndex) {
            auto itemIndex = m_filteredIndexes[visibleIndex];
            auto const& item = m_items[itemIndex];

            float y =
                totalHeight
                - ROW_GAP
                - ROW_H / 2.f
                - static_cast<float>(visibleIndex) * (ROW_H + ROW_GAP);

            auto visual = this->createRowVisual(item, visibleIndex);

            auto button = CCMenuItemSpriteExtra::create(
                visual,
                this,
                menu_selector(LCDListPopup::onLevel)
            );
            button->setTag(static_cast<int>(itemIndex));

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

    void onDifficulty(CCObject*) {
        m_difficultyIndex =
            (m_difficultyIndex + 1) % static_cast<int>(m_difficulties.size());

        m_filterStatus->setString(
            fmt::format(
                "Difficulty: {}",
                m_difficulties[m_difficultyIndex]
            ).c_str()
        );

        this->rebuildList();
    }

    void onTab(CCObject* sender) {
        auto button = static_cast<CCMenuItemSpriteExtra*>(sender);
        m_tab = static_cast<Tab>(button->getTag());
        this->rebuildList();
    }

    void onRandom(CCObject*) {
        this->updateFilteredIndexes();

        if (m_filteredIndexes.empty()) {
            FLAlertLayer::create(
                "Random LCD Level",
                "There are no levels in the current search/filter.",
                "OK"
            )->show();
            return;
        }

        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<std::size_t> pick(
            0,
            m_filteredIndexes.size() - 1
        );

        auto item = m_items[m_filteredIndexes[pick(rng)]];
        auto popup = LCDLevelPopup::create(
            std::move(item),
            [this]() {
                this->rebuildList();
            }
        );

        if (popup) popup->show();
    }

    void onLevel(CCObject* sender) {
        auto button = static_cast<CCMenuItemSpriteExtra*>(sender);
        auto index = static_cast<std::size_t>(button->getTag());

        if (index >= m_items.size()) {
            return;
        }

        auto popup = LCDLevelPopup::create(
            m_items[index],
            [this]() {
                this->rebuildList();
            }
        );

        if (popup) popup->show();
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
