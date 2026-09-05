#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/LoadingSpinner.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>
#include <matjson/std.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
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
        std::string recordId;
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

    int checkedInt(std::intmax_t value) {
        if (value < static_cast<std::intmax_t>(std::numeric_limits<int>::min())
            || value > static_cast<std::intmax_t>(std::numeric_limits<int>::max())) {
            return 0;
        }
        return static_cast<int>(value);
    }

    int intField(matjson::Value const& item, std::string_view key) {
        if (!item.isObject() || !item.contains(key)) return 0;
        return item[key].asInt().map([](std::intmax_t value) {
            return checkedInt(value);
        }).unwrapOr(0);
    }

    int flexibleIntField(matjson::Value const& item, std::string_view key) {
        if (!item.isObject() || !item.contains(key)) return 0;

        auto asInt = item[key].asInt();
        if (!asInt.isErr()) {
            return checkedInt(asInt.unwrap());
        }

        auto asString = item[key].asString();
        if (!asString.isErr()) {
            auto text = asString.unwrap();

            try {
                std::size_t consumed = 0;
                auto parsed = std::stoll(text, &consumed);

                if (consumed != text.size()) {
                    return 0;
                }

                if (parsed < static_cast<long long>(std::numeric_limits<int>::min())
                    || parsed > static_cast<long long>(std::numeric_limits<int>::max())) {
                    return 0;
                }

                return static_cast<int>(parsed);
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
        if (!item.isObject() || !item.contains(key)) return values;

        auto arr = item[key].asArray();
        if (arr.isErr()) return values;

        for (auto const& value : arr.unwrap()) {
            auto text = value.asString();
            if (!text.isErr()) values.push_back(text.unwrap());
        }
        return values;
    }

    std::string join(std::vector<std::string> const& values, std::string const& separator) {
        std::string out;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) out += separator;
            out += values[i];
        }
        return out;
    }

    bool isAllowedVideoUrl(std::string const& rawUrl) {
        auto url = lower(rawUrl);

        // Completion-video buttons are intentionally restricted to YouTube.
        // This prevents a malformed/public website field from turning the mod
        // into an arbitrary external-link launcher.
        return url.starts_with("https://youtu.be/")
            || url.starts_with("https://www.youtube.com/")
            || url.starts_with("https://youtube.com/")
            || url.starts_with("https://m.youtube.com/");
    }

    std::vector<std::string> filterVideoUrls(
        std::vector<std::string> const& urls
    ) {
        std::vector<std::string> out;
        out.reserve(urls.size());

        for (auto const& url : urls) {
            if (isAllowedVideoUrl(url)) out.push_back(url);
        }

        return out;
    }

    std::string sanitizeText(std::string value) {
        std::string out;
        out.reserve(value.size());

        for (char c : value) {
            if (c == '\r') continue;
            if (c == '\0') {
                out += ' ';
                continue;
            }
            // TextArea/FLAlert markup uses angle-bracket tags. User-generated
            // website text should never be able to inject formatting tags.
            if (c == '<') out += '[';
            else if (c == '>') out += ']';
            else if (c == '\t') out += ' ';
            else out += c;
        }

        return out;
    }


    std::string singleLineText(std::string value) {
        for (char& c : value) {
            if (c == '\r' || c == '\n' || c == '\t' || c == '\0') c = ' ';
        }
        return value;
    }

    std::string truncateUtf8(std::string value, std::size_t maxBytes) {
        if (value.size() <= maxBytes) return value;
        if (maxBytes <= 3) return value.substr(0, maxBytes);

        std::size_t cut = maxBytes - 3;
        // Never split a UTF-8 continuation byte.
        while (cut > 0
            && (static_cast<unsigned char>(value[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }

        value.resize(cut);
        value += "...";
        return value;
    }

    // Stable identity for list-history snapshots. Base44's record ID is the
    // best identifier here because rank/title/level_id can all be edited.
    std::string snapshotKey(RankItemData const& item) {
        if (!item.recordId.empty()) {
            return "record:" + item.recordId;
        }
        if (item.levelId > 0) {
            return fmt::format("gd:{}", item.levelId);
        }
        return "name:" + lower(item.title) + "|creator:" + lower(item.creator);
    }

    // Stable identity for PERSONAL data. Prefer Base44's immutable record ID so
    // correcting/changing level_id later does not wipe local progress. level_id
    // remains the only value used for opening the Geometry Dash level itself.
    std::string personalKey(RankItemData const& item) {
        if (!item.recordId.empty()) {
            return "record:" + item.recordId;
        }
        if (item.levelId > 0) {
            return fmt::format("gd:{}", item.levelId);
        }
        return "name:" + lower(item.title) + "|creator:" + lower(item.creator);
    }

    std::string nameIdentity(RankItemData const& item) {
        return "name:" + lower(item.title) + "|creator:" + lower(item.creator);
    }

    // Known identities used by older development builds. Checking all of them
    // makes progress/favorites survive upgrades and the later addition of level_id.
    std::vector<std::string> legacyPersonalIdentities(RankItemData const& item) {
        std::vector<std::string> keys;

        if (item.levelId > 0) {
            // v1.4.4 and earlier ID-ready builds.
            keys.push_back(fmt::format("id:{}", item.levelId));
            // Included for forward/backward compatibility with this verified schema.
            keys.push_back(fmt::format("gd:{}", item.levelId));
        }

        if (!item.recordId.empty()) {
            // Internal audit builds briefly preferred Base44 record IDs.
            keys.push_back("record:" + item.recordId);
        }

        // Builds made before a level had level_id used title+creator.
        keys.push_back(nameIdentity(item));
        return keys;
    }

    std::string progressKey(RankItemData const& item) {
        return "progress::" + personalKey(item);
    }

    std::string completedKey(RankItemData const& item) {
        return "completed::" + personalKey(item);
    }

    std::string favoriteKey(RankItemData const& item) {
        return "favorite::" + personalKey(item);
    }

    std::string preCompleteProgressKey(RankItemData const& item) {
        return "pre-complete-progress::" + personalKey(item);
    }

    template <class T>
    T getMigratedSavedValue(
        std::string const& currentKey,
        std::string_view prefix,
        RankItemData const& item
    ) {
        auto mod = Mod::get();

        if (!mod->hasSavedValue(currentKey)) {
            for (auto const& identity : legacyPersonalIdentities(item)) {
                auto oldKey = std::string(prefix) + identity;
                if (oldKey == currentKey || !mod->hasSavedValue(oldKey)) {
                    continue;
                }

                mod->setSavedValue<T>(
                    currentKey,
                    mod->getSavedValue<T>(oldKey)
                );
                break;
            }
        }

        return mod->getSavedValue<T>(currentKey);
    }

    int getProgress(RankItemData const& item) {
        return getMigratedSavedValue<int>(
            progressKey(item),
            "progress::",
            item
        );
    }

    bool isCompleted(RankItemData const& item) {
        auto saved = getMigratedSavedValue<bool>(
            completedKey(item),
            "completed::",
            item
        );
        return saved || getProgress(item) >= 100;
    }

    bool isFavorite(RankItemData const& item) {
        return getMigratedSavedValue<bool>(
            favoriteKey(item),
            "favorite::",
            item
        );
    }

    int getPreCompleteProgress(RankItemData const& item) {
        return getMigratedSavedValue<int>(
            preCompleteProgressKey(item),
            "pre-complete-progress::",
            item
        );
    }

    void setProgress(RankItemData const& item, int value) {
        value = std::clamp(value, 0, 100);
        Mod::get()->setSavedValue<int>(progressKey(item), value);

        // Keep completion and personal best synchronized:
        // 100% = completed, anything below 100% = not completed.
        Mod::get()->setSavedValue<bool>(
            completedKey(item),
            value >= 100
        );
    }

    std::optional<std::vector<RankItemData>> parseItems(
        matjson::Value const& json
    ) {
        if (!json.isObject() || !json.contains("items")) {
            return std::nullopt;
        }

        auto arr = json["items"].asArray();
        if (arr.isErr()) {
            return std::nullopt;
        }

        auto values = arr.unwrap();
        std::vector<RankItemData> parsed;
        parsed.reserve(values.size());

        for (auto const& item : values) {
            // Ignore malformed array entries rather than creating fake
            // "Untitled" rows from non-object JSON values.
            if (!item.isObject()) continue;

            RankItemData data;
            data.rank = std::max(0, intField(item, "rank"));
            data.levelId = std::max(0, flexibleIntField(item, "level_id"));
            // Base44's own record ID: used only to recognize the same RankItem
            // across rank/title edits. Never used as a Geometry Dash level ID.
            data.recordId = stringField(item, "id");
            data.title = singleLineText(stringField(item, "title"));
            data.creator = singleLineText(stringField(item, "creator"));
            data.difficulty = singleLineText(stringField(item, "difficulty"));
            data.description = stringField(item, "description");
            data.createdDate = stringField(item, "created_date");
            data.beatenBy = stringArrayField(item, "beaten_by");
            data.youtubeUrls = filterVideoUrls(
                stringArrayField(item, "youtube_urls")
            );

            if (data.title.empty()) data.title = "Untitled";
            parsed.push_back(std::move(data));
        }

        // An empty items array is a valid empty list. A non-empty array where
        // every entry was malformed is not; let the caller fall back to cache.
        if (!values.empty() && parsed.empty()) {
            return std::nullopt;
        }

        std::sort(parsed.begin(), parsed.end(), [](auto const& a, auto const& b) {
            if (a.rank == 0 && b.rank == 0) return a.title < b.title;
            if (a.rank == 0) return false;
            if (b.rank == 0) return true;
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.title < b.title;
        });

        return parsed;
    }

    void openGDLevel(RankItemData const& item) {
        if (item.levelId <= 0) {
            FLAlertLayer::create(
                "No Level ID",
                "This level does not have a <cy>level_id</c> on LCD yet.",
                "OK"
            )->show();
            return;
        }

        auto query = fmt::format("{}", item.levelId);
        auto search = GJSearchObject::create(
            SearchType::Search,
            query
        );

        if (!search) return;

        auto scene = LevelBrowserLayer::scene(search);
        CCDirector::sharedDirector()->replaceScene(
            CCTransitionFade::create(.30f, scene)
        );
    }

    CCLabelBMFont* makeLabel(
        std::string const& text,
        char const* font,
        float scale,
        CCPoint pos,
        CCPoint anchor = {0.5f, 0.5f}
    ) {
        auto label = CCLabelBMFont::create(text.c_str(), font);
        label->setScale(scale);
        label->setPosition(pos);
        label->setAnchorPoint(anchor);
        return label;
    }

    CCMenu* makeAbsoluteMenu(CCSize const& size) {
        auto menu = CCMenu::create();
        menu->setContentSize(size);
        menu->ignoreAnchorPointForPosition(false);
        menu->setAnchorPoint({0.f, 0.f});
        menu->setPosition({0.f, 0.f});
        return menu;
    }

    int gdLevelId(GJGameLevel* level) {
        if (!level) return 0;
        return std::max(0, level->m_levelID.value());
    }

    std::optional<std::vector<RankItemData>> loadCachedRankItems() {
        auto cached = Mod::get()->getSavedValue<std::string>("cache-json");
        if (cached.empty()) return std::nullopt;

        auto json = matjson::Value::parse(cached);
        if (json.isErr()) return std::nullopt;
        return parseItems(json.unwrap());
    }

    std::optional<RankItemData> findRankItemByLevelId(
        std::vector<RankItemData> const& items,
        int levelId
    ) {
        if (levelId <= 0) return std::nullopt;

        auto it = std::find_if(items.begin(), items.end(), [levelId](auto const& item) {
            return item.levelId == levelId;
        });

        if (it == items.end()) return std::nullopt;
        return *it;
    }

    std::optional<RankItemData> findCachedRankItem(int levelId) {
        auto items = loadCachedRankItems();
        if (!items.has_value()) return std::nullopt;
        return findRankItemByLevelId(*items, levelId);
    }

    std::unordered_map<std::string, int> savedRankDeltas() {
        std::unordered_map<std::string, int> out;
        auto lines = Mod::get()->getSavedValue<std::vector<std::string>>("rank-deltas");

        for (auto const& line : lines) {
            auto pos = line.rfind('\x1e');
            if (pos == std::string::npos) continue;

            try {
                out[line.substr(0, pos)] = std::stoi(line.substr(pos + 1));
            } catch (...) {
                // Ignore malformed saved state.
            }
        }
        return out;
    }

    int rankDeltaFor(RankItemData const& item) {
        auto deltas = savedRankDeltas();
        auto it = deltas.find(snapshotKey(item));
        return it == deltas.end() ? 0 : it->second;
    }

    std::string rankText(RankItemData const& item, bool includeLCD = false) {
        std::string out = includeLCD ? "LCD " : "";
        out += item.rank > 0 ? fmt::format("#{}", item.rank) : "Unranked";

        auto delta = rankDeltaFor(item);
        if (delta > 0) out += fmt::format(" +{}", delta);
        else if (delta < 0) out += fmt::format(" {}", delta);
        return out;
    }

    void syncProgressFromGD(RankItemData const& item, GJGameLevel* level) {
        if (!level) return;
        auto best = std::clamp(level->getNormalPercent(), 0, 100);
        if (best > getProgress(item)) {
            setProgress(item, best);
        }
    }

    std::optional<RankItemData> nextUncompletedLCD(
        RankItemData const& current,
        std::vector<RankItemData> const& items
    ) {
        if (items.empty()) return std::nullopt;

        std::size_t start = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].levelId == current.levelId) {
                start = i + 1;
                break;
            }
        }

        for (std::size_t offset = 0; offset < items.size(); ++offset) {
            auto index = (start + offset) % items.size();
            auto const& item = items[index];
            if (item.levelId > 0 && item.levelId != current.levelId && !isCompleted(item)) {
                return item;
            }
        }
        return std::nullopt;
    }

    std::optional<RankItemData> randomLCD(
        RankItemData const& current,
        std::vector<RankItemData> const& items
    ) {
        std::vector<RankItemData> candidates;
        for (auto const& item : items) {
            if (item.levelId > 0 && item.levelId != current.levelId) {
                candidates.push_back(item);
            }
        }
        if (candidates.empty()) return std::nullopt;

        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<std::size_t> pick(0, candidates.size() - 1);
        return candidates[pick(rng)];
    }
}

/* =========================================================
   CLEAN LEVEL DETAILS POPUP
   ========================================================= */

class LCDLevelPopup : public geode::Popup {
protected:
    RankItemData m_item;
    std::function<void()> m_onChanged;

    TextInput* m_progressInput = nullptr;
    CCLabelBMFont* m_progressState = nullptr;
    CCMenuItemSpriteExtra* m_favoriteButton = nullptr;
    CCMenuItemSpriteExtra* m_completedButton = nullptr;
    CCMenuItemSpriteExtra* m_videoButton = nullptr;
    std::size_t m_videoIndex = 0;

    bool init(RankItemData item, std::function<void()> onChanged) {
        if (!Popup::init(440.f, 270.f)) return false;

        m_item = std::move(item);
        m_onChanged = std::move(onChanged);

        this->setTitle(m_item.title.c_str());

        auto size = m_mainLayer->getContentSize();

        // Clean metadata line
        auto meta = fmt::format(
            "{}   {}   {}",
            rankText(m_item),
            m_item.creator.empty() ? "Unknown creator" : m_item.creator,
            m_item.difficulty.empty() ? "Unknown difficulty" : m_item.difficulty
        );

        auto metaLabel = makeLabel(
            meta,
            "goldFont.fnt",
            .38f,
            {size.width / 2.f, size.height - 44.f}
        );
        metaLabel->limitLabelWidth(365.f, .38f, .23f);
        m_mainLayer->addChild(metaLabel);

        // Description/details panel
        auto detailsPanel = CCLayerColor::create(
            ccc4(0, 0, 0, 55),
            382.f,
            105.f
        );
        detailsPanel->setPosition({29.f, 94.f});
        m_mainLayer->addChild(detailsPanel);

        std::string info;
        if (!m_item.description.empty()) {
            auto description = truncateUtf8(
                singleLineText(sanitizeText(m_item.description)),
                170
            );
            info += description;
        } else {
            info += "No description has been added for this level yet.";
        }

        if (!m_item.beatenBy.empty()) {
            auto beaten = truncateUtf8(
                singleLineText(sanitizeText(join(m_item.beatenBy, ", "))),
                60
            );
            info += "\n\nBeaten by: " + beaten;
        }

        info = truncateUtf8(info, 240);

        auto text = TextArea::create(
            info.c_str(),
            "chatFont.fnt",
            .43f,
            355.f,
            {0.5f, 1.f},
            15.f,
            true
        );
        text->setPosition({size.width / 2.f, size.height - 76.f});
        m_mainLayer->addChild(text);

        // Progress section, separated from actions
        auto progressTitle = makeLabel(
            "PERSONAL PROGRESS",
            "goldFont.fnt",
            .30f,
            {36.f, 78.f},
            {0.f, .5f}
        );
        m_mainLayer->addChild(progressTitle);

        m_progressInput = TextInput::create(72.f, "0-100", "bigFont.fnt");
        m_progressInput->setFilter("0123456789");
        m_progressInput->setMaxCharCount(3);
        {
            auto initialProgress = fmt::format("{}", getProgress(m_item));
            m_progressInput->setString(initialProgress.c_str(), false);
        }
        m_progressInput->setPosition({74.f, 54.f});
        m_mainLayer->addChild(m_progressInput);

        m_progressState = makeLabel(
            "",
            "goldFont.fnt",
            .32f,
            {118.f, 54.f},
            {0.f, .5f}
        );
        m_mainLayer->addChild(m_progressState);

        auto menu = makeAbsoluteMenu(size);
        m_mainLayer->addChild(menu);

        // One large primary button
        auto play = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(m_item.levelId > 0 ? "PLAY LEVEL" : "LEVEL ID MISSING"),
            this,
            menu_selector(LCDLevelPopup::onPlay)
        );
        play->setScale(.72f);
        play->setPosition({322.f, 66.f});
        menu->addChild(play);

        // Secondary actions are smaller and grouped
        auto save = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Save %"),
            this,
            menu_selector(LCDLevelPopup::onSaveProgress)
        );
        save->setScale(.50f);
        save->setPosition({70.f, 25.f});
        menu->addChild(save);

        m_favoriteButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(isFavorite(m_item) ? "Favorited" : "Favorite"),
            this,
            menu_selector(LCDLevelPopup::onFavorite)
        );
        m_favoriteButton->setScale(.50f);
        m_favoriteButton->setPosition({160.f, 25.f});
        menu->addChild(m_favoriteButton);

        m_completedButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(isCompleted(m_item) ? "Completed" : "Mark Done"),
            this,
            menu_selector(LCDLevelPopup::onCompleted)
        );
        m_completedButton->setScale(.50f);
        m_completedButton->setPosition({260.f, 25.f});
        menu->addChild(m_completedButton);

        m_videoButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Video"),
            this,
            menu_selector(LCDLevelPopup::onVideo)
        );
        m_videoButton->setScale(.50f);
        m_videoButton->setPosition({355.f, 25.f});
        menu->addChild(m_videoButton);

        this->refreshVideoButton();
        this->refreshProgressText();
        return true;
    }

    void refreshVideoButton() {
        if (!m_videoButton) return;

        std::string label;
        if (m_item.youtubeUrls.empty()) {
            label = "No Video";
            m_videoIndex = 0;
        } else if (m_item.youtubeUrls.size() == 1) {
            label = "Video";
            m_videoIndex = 0;
        } else {
            m_videoIndex %= m_item.youtubeUrls.size();
            label = fmt::format(
                "Video {}/{}",
                m_videoIndex + 1,
                m_item.youtubeUrls.size()
            );
        }

        if (auto sprite = typeinfo_cast<ButtonSprite*>(m_videoButton->getNormalImage())) {
            sprite->setString(label.c_str());
            m_videoButton->updateSprite();
        }
    }

    void refreshProgressText() {
        auto pct = getProgress(m_item);
        std::string state = isCompleted(m_item)
            ? fmt::format("{}%  -  COMPLETE", pct)
            : fmt::format("{}%  -  IN PROGRESS", pct);

        if (m_progressState) m_progressState->setString(state.c_str());
    }

    void changed() {
        this->refreshProgressText();
        if (m_onChanged) m_onChanged();
    }

    void onPlay(CCObject*) {
        openGDLevel(m_item);
    }

    void onVideo(CCObject*) {
        if (m_item.youtubeUrls.empty()) {
            FLAlertLayer::create(
                "LCD",
                "No valid YouTube completion video is attached.",
                "OK"
            )->show();
            return;
        }

        m_videoIndex %= m_item.youtubeUrls.size();
        auto url = m_item.youtubeUrls[m_videoIndex];

        // Advance the label for the next tap before leaving the game.
        if (m_item.youtubeUrls.size() > 1) {
            m_videoIndex = (m_videoIndex + 1) % m_item.youtubeUrls.size();
            this->refreshVideoButton();
        }

        geode::utils::web::openLinkInBrowser(url.c_str());
    }

    void onSaveProgress(CCObject*) {
        int value = 0;
        try {
            value = std::stoi(std::string(m_progressInput->getString().c_str()));
        } catch (...) {
            value = 0;
        }

        value = std::clamp(value, 0, 100);

        auto oldProgress = getProgress(m_item);
        if (value == 100 && oldProgress < 100) {
            Mod::get()->setSavedValue<int>(
                preCompleteProgressKey(m_item),
                oldProgress
            );
        }

        setProgress(m_item, value);
        {
            auto progressText = fmt::format("{}", value);
            m_progressInput->setString(progressText.c_str(), false);
        }

        if (auto sprite = typeinfo_cast<ButtonSprite*>(m_completedButton->getNormalImage())) {
            sprite->setString(isCompleted(m_item) ? "Completed" : "Mark Done");
            m_completedButton->updateSprite();
        }

        this->changed();
    }

    void onFavorite(CCObject*) {
        auto next = !isFavorite(m_item);
        Mod::get()->setSavedValue<bool>(favoriteKey(m_item), next);

        if (auto sprite = typeinfo_cast<ButtonSprite*>(m_favoriteButton->getNormalImage())) {
            sprite->setString(next ? "Favorited" : "Favorite");
            m_favoriteButton->updateSprite();
        }
        this->changed();
    }

    void onCompleted(CCObject*) {
        auto currentlyCompleted = isCompleted(m_item);

        if (!currentlyCompleted) {
            // Remember the previous best so "Completed" can be undone cleanly.
            Mod::get()->setSavedValue<int>(
                preCompleteProgressKey(m_item),
                getProgress(m_item)
            );

            setProgress(m_item, 100);
            m_progressInput->setString("100", false);
        } else {
            auto currentProgress = getProgress(m_item);
            auto previousBest = getPreCompleteProgress(m_item);

            // Older builds may have a completed flag without a stored
            // pre-completion value. In that case, preserve an existing
            // sub-100 personal best instead of unexpectedly resetting to 0.
            if (previousBest == 0 && currentProgress > 0 && currentProgress < 100) {
                previousBest = currentProgress;
            }

            previousBest = std::clamp(previousBest, 0, 99);

            // Restore the user's previous personal best and clear completion.
            Mod::get()->setSavedValue<int>(progressKey(m_item), previousBest);
            Mod::get()->setSavedValue<bool>(completedKey(m_item), false);
            auto restoredText = fmt::format("{}", previousBest);
            m_progressInput->setString(restoredText.c_str(), false);
        }

        auto nowCompleted = isCompleted(m_item);

        if (auto sprite = typeinfo_cast<ButtonSprite*>(m_completedButton->getNormalImage())) {
            sprite->setString(nowCompleted ? "Completed" : "Mark Done");
            m_completedButton->updateSprite();
        }

        this->changed();
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

/* =========================================================
   CLEAN MAIN LCD BROWSER
   ========================================================= */

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
    TextInput* m_search = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_resultCount = nullptr;
    CCLabelBMFont* m_filterLabel = nullptr;

    std::vector<RankItemData> m_items;
    std::vector<std::size_t> m_visible;
    std::unordered_set<std::string> m_recentNewKeys;

    Tab m_tab = Tab::All;
    std::string m_query;
    std::size_t m_difficultyIndex = 0;

    std::vector<std::string> m_difficulties {
        "All", "Extreme", "Insane", "Hard", "Medium", "Easy"
    };

    static constexpr float W = 520.f;
    static constexpr float H = 300.f;
    static constexpr float LIST_W = 472.f;
    static constexpr float LIST_H = 125.f;
    static constexpr float ROW_H = 37.f;
    static constexpr float ROW_GAP = 3.f;

    bool init() {
        if (!Popup::init(W, H)) return false;

        this->setTitle("LCD Community List");

        auto size = m_mainLayer->getContentSize();

        // Row 1: LIVE/SYNC status on the left, visible level count on the right
        m_status = makeLabel(
            "Connecting...",
            "goldFont.fnt",
            .30f,
            {25.f, size.height - 42.f},
            {0.f, .5f}
        );
        m_mainLayer->addChild(m_status);

        m_resultCount = makeLabel(
            "",
            "goldFont.fnt",
            .30f,
            {size.width - 25.f, size.height - 42.f},
            {1.f, .5f}
        );
        m_mainLayer->addChild(m_resultCount);

        // Row 2: wide search field + Filter / Random / Sync
        m_search = TextInput::create(265.f, "Search levels or creators...", "bigFont.fnt");
        m_search->setMaxCharCount(40);
        m_search->setTextAlign(geode::TextInputAlign::Left);
        m_search->setPosition({165.f, size.height - 71.f});
        m_search->setCallback([this](std::string const& value) {
            m_query = lower(value);
            this->rebuildList();
        });
        m_mainLayer->addChild(m_search);

        // Only 3 compact utility buttons on this row.
        // This menu is normalized to the popup's coordinate space.
        auto utilityMenu = makeAbsoluteMenu(size);
        m_mainLayer->addChild(utilityMenu);

        auto filter = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Filter"),
            this,
            menu_selector(LCDListPopup::onDifficulty)
        );
        filter->setScale(.52f);
        filter->setPosition({335.f, size.height - 71.f});
        utilityMenu->addChild(filter);

        auto random = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Random"),
            this,
            menu_selector(LCDListPopup::onRandom)
        );
        random->setScale(.52f);
        random->setPosition({407.f, size.height - 71.f});
        utilityMenu->addChild(random);

        auto refresh = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Sync"),
            this,
            menu_selector(LCDListPopup::onRefresh)
        );
        refresh->setScale(.52f);
        refresh->setPosition({474.f, size.height - 71.f});
        utilityMenu->addChild(refresh);

        m_filterLabel = makeLabel(
            "Difficulty: All",
            "goldFont.fnt",
            .28f,
            {25.f, size.height - 93.f},
            {0.f, .5f}
        );
        m_mainLayer->addChild(m_filterLabel);

        // Row 4: ALL / TOP 10 / NEW / FAVORITES / DONE
        auto tabMenu = makeAbsoluteMenu(size);
        m_mainLayer->addChild(tabMenu);

        struct TabSpec { char const* text; Tab tab; float x; };
        std::vector<TabSpec> tabs {
            {"ALL", Tab::All, 72.f},
            {"TOP 10", Tab::Top, 163.f},
            {"NEW", Tab::New, 260.f},
            {"FAVORITES", Tab::Favorites, 360.f},
            {"DONE", Tab::Completed, 455.f}
        };

        for (auto const& spec : tabs) {
            auto button = CCMenuItemSpriteExtra::create(
                ButtonSprite::create(spec.text),
                this,
                menu_selector(LCDListPopup::onTab)
            );
            button->setScale(.47f);
            button->setTag(static_cast<int>(spec.tab));
            button->setPosition({spec.x, size.height - 118.f});
            tabMenu->addChild(button);
        }

        // Row 5: dedicated list panel below the tabs with a visible gap
        auto listPanel = CCLayerColor::create(
            ccc4(0, 0, 0, 45),
            LIST_W + 8.f,
            LIST_H + 8.f
        );
        listPanel->setPosition({20.f, 25.f});
        m_mainLayer->addChild(listPanel);

        m_scroll = ScrollLayer::create(
            CCSize{LIST_W, LIST_H},
            true,
            true
        );
        m_scroll->setPosition({24.f, 29.f});
        m_scroll->setStealingTouches(true);
        m_mainLayer->addChild(m_scroll);

        m_spinner = LoadingSpinner::create(32.f);
        m_spinner->setPosition({size.width / 2.f, 29.f + LIST_H / 2.f});
        m_mainLayer->addChild(m_spinner, 20);

        this->loadRecentNewKeys();
        this->fetchLevels();
        return true;
    }

    void loadRecentNewKeys() {
        auto values = Mod::get()->getSavedValue<std::vector<std::string>>(
            "recent-new-keys"
        );
        m_recentNewKeys = std::unordered_set<std::string>(
            values.begin(),
            values.end()
        );
    }

    bool isNew(RankItemData const& item) const {
        return m_recentNewKeys.contains(snapshotKey(item));
    }

    std::vector<std::string> makeSnapshot(
        std::vector<RankItemData> const& items
    ) const {
        std::vector<std::string> out;
        for (auto const& item : items) {
            out.push_back(fmt::format("{}\x1e{}", snapshotKey(item), item.rank));
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

            int rank = 0;
            try { rank = std::stoi(line.substr(pos + 1)); }
            catch (...) { rank = 0; }

            out[line.substr(0, pos)] = rank;
        }
        return out;
    }

    void detectChanges(std::vector<RankItemData> const& fresh) {
        auto newSnapshot = this->makeSnapshot(fresh);

        // v1.4.4 starts using Base44 record IDs for snapshot identity.
        // Establish one clean baseline so upgrading from older versions does not
        // falsely report every level as removed + new.
        if (!Mod::get()->getSavedValue<bool>("snapshot-record-id-schema")) {
            Mod::get()->setSavedValue("list-snapshot", newSnapshot);
            Mod::get()->setSavedValue(
                "recent-new-keys",
                std::vector<std::string>{}
            );
            Mod::get()->setSavedValue(
                "rank-deltas",
                std::vector<std::string>{}
            );
            m_recentNewKeys.clear();
            Mod::get()->setSavedValue("snapshot-record-id-schema", true);
            return;
        }

        auto oldSnapshot =
            Mod::get()->getSavedValue<std::vector<std::string>>("list-snapshot");

        auto oldMap = this->snapshotMap(oldSnapshot);
        auto newMap = this->snapshotMap(newSnapshot);

        // If the schema flag exists but the snapshot is missing/corrupt, rebuild
        // a clean baseline instead of reporting a fake mass update.
        if (oldMap.empty() && !newMap.empty()) {
            Mod::get()->setSavedValue("list-snapshot", newSnapshot);
            Mod::get()->setSavedValue(
                "recent-new-keys",
                std::vector<std::string>{}
            );
            Mod::get()->setSavedValue(
                "rank-deltas",
                std::vector<std::string>{}
            );
            m_recentNewKeys.clear();
            return;
        }

        std::vector<std::string> added;
        std::vector<std::string> rankDeltas;
        int moved = 0;
        int removed = 0;

        if (!oldMap.empty()) {
            for (auto const& [key, rank] : newMap) {
                auto old = oldMap.find(key);
                if (old == oldMap.end()) {
                    added.push_back(key);
                } else if (old->second != rank) {
                    ++moved;
                    // Positive = moved UP the list (example #9 -> #6 = +3).
                    auto delta = old->second - rank;
                    rankDeltas.push_back(fmt::format("{}\x1e{}", key, delta));
                }
            }

            for (auto const& [key, rank] : oldMap) {
                if (!newMap.contains(key)) ++removed;
            }
        }

        Mod::get()->setSavedValue("list-snapshot", newSnapshot);

        auto changed = !added.empty() || moved > 0 || removed > 0;
        if (changed) {
            Mod::get()->setSavedValue("recent-new-keys", added);
            Mod::get()->setSavedValue("rank-deltas", rankDeltas);
            m_recentNewKeys = std::unordered_set<std::string>(
                added.begin(),
                added.end()
            );
        }

        if (!oldMap.empty() && changed) {
            auto updateMessage = fmt::format(
                "<cg>{} new</c>   <cy>{} moved</c>   <cr>{} removed</c>",
                added.size(), moved, removed
            );

            FLAlertLayer::create(
                "LCD Updated",
                updateMessage.c_str(),
                "OK"
            )->show();
        }
    }

    void setLoading(bool loading) {
        if (m_spinner) m_spinner->setVisible(loading);
        if (m_scroll) m_scroll->setVisible(!loading);
    }

    bool applyJson(matjson::Value const& json, bool cached) {
        auto parsed = parseItems(json);
        if (!parsed.has_value()) return false;

        m_items = std::move(*parsed);

        if (!cached) {
            this->detectChanges(m_items);
            Mod::get()->setSavedValue<std::string>(
                "cache-json",
                json.dump(matjson::NO_INDENTATION)
            );
        }

        this->setLoading(false);
        this->rebuildList();

        m_status->setString(
            cached ? "OFFLINE CACHE" : "LIVE - SYNCED"
        );

        return true;
    }

    bool loadCache() {
        auto cached = Mod::get()->getSavedValue<std::string>("cache-json");
        if (cached.empty()) return false;

        auto parsed = matjson::Value::parse(cached);
        if (parsed.isErr()) return false;

        return this->applyJson(parsed.unwrap(), true);
    }

    void fetchLevels() {
        this->setLoading(true);
        m_status->setString("SYNCING...");

        auto request = web::WebRequest();
        request.timeout(std::chrono::seconds(15));
        request.userAgent("LCD-Geode-Mod/1.5.1");

        m_request.spawn(
            "Sync LCD Community List",
            request.get(API_URL),
            [this](web::WebResponse response) {
                if (!response.ok()) {
                    if (this->loadCache()) return;

                    this->setLoading(false);
                    m_status->setString("OFFLINE - NO CACHE");
                    return;
                }

                auto json = response.json();
                if (json.isErr() || !this->applyJson(json.unwrap(), false)) {
                    if (this->loadCache()) return;

                    this->setLoading(false);
                    m_status->setString("INVALID RESPONSE");
                }
            }
        );
    }

    bool passesTab(RankItemData const& item) const {
        switch (m_tab) {
            case Tab::All: return true;
            case Tab::Top: return item.rank > 0 && item.rank <= 10;
            case Tab::New: return this->isNew(item);
            case Tab::Favorites: return isFavorite(item);
            case Tab::Completed: return isCompleted(item);
            default: return true;
        }
    }

    bool passesDifficulty(RankItemData const& item) const {
        if (m_difficultyIndex == 0) return true;

        auto wanted = lower(m_difficulties[m_difficultyIndex]);
        return lower(item.difficulty).find(wanted) != std::string::npos;
    }

    bool passesSearch(RankItemData const& item) const {
        if (m_query.empty()) return true;

        return lower(item.title).find(m_query) != std::string::npos
            || lower(item.creator).find(m_query) != std::string::npos
            || lower(item.difficulty).find(m_query) != std::string::npos;
    }

    void updateVisible() {
        m_visible.clear();

        for (std::size_t i = 0; i < m_items.size(); ++i) {
            auto const& item = m_items[i];
            if (passesTab(item) && passesDifficulty(item) && passesSearch(item)) {
                m_visible.push_back(i);
            }
        }

        if (m_resultCount) {
            m_resultCount->setString(
                fmt::format("{} LEVELS", m_visible.size()).c_str()
            );
        }
    }

    CCNode* createRow(RankItemData const& item, std::size_t visibleIndex) {
        auto row = CCLayerColor::create(
            ccc4(0, 0, 0, visibleIndex % 2 ? 30 : 58),
            LIST_W - 5.f,
            ROW_H
        );

        // Rank column
        auto rank = makeLabel(
            item.rank > 0 ? fmt::format("#{}", item.rank) : "-",
            "bigFont.fnt",
            .47f,
            {13.f, ROW_H / 2.f},
            {0.f, .5f}
        );
        row->addChild(rank);

        // Main title
        auto title = makeLabel(
            item.title,
            "bigFont.fnt",
            .45f,
            {61.f, ROW_H / 2.f + 6.f},
            {0.f, .5f}
        );
        title->limitLabelWidth(230.f, .45f, .22f);
        row->addChild(title);

        // Small metadata below it
        auto subtitleText = fmt::format(
            "{}  -  {}",
            item.creator.empty() ? "Unknown creator" : item.creator,
            item.difficulty.empty() ? "Unknown difficulty" : item.difficulty
        );

        auto subtitle = makeLabel(
            subtitleText,
            "goldFont.fnt",
            .25f,
            {61.f, ROW_H / 2.f - 7.f},
            {0.f, .5f}
        );
        subtitle->limitLabelWidth(255.f, .25f, .15f);
        row->addChild(subtitle);

        // Quiet badges on the right
        float badgeX = 333.f;

        auto delta = rankDeltaFor(item);
        if (delta != 0) {
            auto deltaLabel = makeLabel(
                delta > 0 ? fmt::format("+{}", delta) : fmt::format("{}", delta),
                "goldFont.fnt",
                .25f,
                {badgeX, ROW_H / 2.f},
                {0.f, .5f}
            );
            row->addChild(deltaLabel);
            badgeX += 37.f;
        }

        if (this->isNew(item)) {
            auto badge = makeLabel(
                "NEW",
                "goldFont.fnt",
                .25f,
                {badgeX, ROW_H / 2.f},
                {0.f, .5f}
            );
            row->addChild(badge);
            badgeX += 37.f;
        }

        if (isFavorite(item)) {
            auto badge = makeLabel(
                "FAV",
                "goldFont.fnt",
                .25f,
                {badgeX, ROW_H / 2.f},
                {0.f, .5f}
            );
            row->addChild(badge);
            badgeX += 37.f;
        }

        auto progress = getProgress(item);
        auto progressText = isCompleted(item)
            ? "100%"
            : progress > 0
                ? fmt::format("{}%", progress)
                : "--";

        auto progressLabel = makeLabel(
            progressText,
            isCompleted(item) ? "goldFont.fnt" : "bigFont.fnt",
            .32f,
            {420.f, ROW_H / 2.f}
        );
        row->addChild(progressLabel);

        auto arrow = makeLabel(
            ">",
            "bigFont.fnt",
            .42f,
            {451.f, ROW_H / 2.f}
        );
        row->addChild(arrow);

        return row;
    }

    void rebuildList() {
        if (!m_scroll) return;

        this->updateVisible();

        auto content = m_scroll->m_contentLayer;
        content->removeAllChildrenWithCleanup(true);

        if (m_visible.empty()) {
            content->setContentSize({LIST_W, LIST_H});
            auto empty = makeLabel(
                "No levels match this view.",
                "bigFont.fnt",
                .48f,
                {LIST_W / 2.f, LIST_H / 2.f}
            );
            content->addChild(empty);
            m_scroll->scrollToTop();
            return;
        }

        float total =
            static_cast<float>(m_visible.size()) * (ROW_H + ROW_GAP) + ROW_GAP;

        total = std::max(total, LIST_H);
        content->setContentSize({LIST_W, total});

        // A single menu owns all row hit areas. This avoids stacking dozens of
        // overlapping CCMenu layers inside the ScrollLayer.
        auto rowMenu = makeAbsoluteMenu(CCSize{LIST_W, total});
        content->addChild(rowMenu, 2);

        for (std::size_t i = 0; i < m_visible.size(); ++i) {
            auto itemIndex = m_visible[i];
            auto const& item = m_items[itemIndex];

            float y =
                total
                - ROW_GAP
                - ROW_H / 2.f
                - static_cast<float>(i) * (ROW_H + ROW_GAP);

            // Keep the exact clean row visual from the mockup.
            auto visual = this->createRow(item, i);
            visual->setPosition({2.5f, y - ROW_H / 2.f});
            content->addChild(visual);

            // Two invisible hit areas:
            // - main/title area => opens that GD level by level_id
            // - arrow area      => opens LCD details
            auto playHitbox = CCLayerColor::create(
                ccc4(0, 0, 0, 0),
                LIST_W - 42.f,
                ROW_H
            );

            auto playButton = CCMenuItemSpriteExtra::create(
                playHitbox,
                this,
                menu_selector(LCDListPopup::onPlayLevel)
            );
            playButton->setTag(static_cast<int>(itemIndex));
            playButton->setPosition({
                (LIST_W - 42.f) / 2.f,
                y
            });
            rowMenu->addChild(playButton);

            auto detailsHitbox = CCLayerColor::create(
                ccc4(0, 0, 0, 0),
                42.f,
                ROW_H
            );

            auto detailsButton = CCMenuItemSpriteExtra::create(
                detailsHitbox,
                this,
                menu_selector(LCDListPopup::onLevel)
            );
            detailsButton->setTag(static_cast<int>(itemIndex));
            detailsButton->setPosition({
                LIST_W - 21.f,
                y
            });
            rowMenu->addChild(detailsButton);
        }

        m_scroll->scrollToTop();
    }

    void onRefresh(CCObject*) {
        this->fetchLevels();
    }

    void onDifficulty(CCObject*) {
        m_difficultyIndex =
            (m_difficultyIndex + 1) % m_difficulties.size();

        m_filterLabel->setString(
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
        this->updateVisible();

        if (m_visible.empty()) {
            FLAlertLayer::create(
                "Random Level",
                "There are no levels in the current view.",
                "OK"
            )->show();
            return;
        }

        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<std::size_t> pick(0, m_visible.size() - 1);

        auto popup = LCDLevelPopup::create(
            m_items[m_visible[pick(rng)]],
            [this]() { this->rebuildList(); }
        );

        if (popup) popup->show();
    }

    void onPlayLevel(CCObject* sender) {
        auto button = static_cast<CCMenuItemSpriteExtra*>(sender);
        auto index = static_cast<std::size_t>(button->getTag());
        if (index >= m_items.size()) return;

        openGDLevel(m_items[index]);
    }

    void onLevel(CCObject* sender) {
        auto button = static_cast<CCMenuItemSpriteExtra*>(sender);
        auto index = static_cast<std::size_t>(button->getTag());
        if (index >= m_items.size()) return;

        auto popup = LCDLevelPopup::create(
            m_items[index],
            [this]() { this->rebuildList(); }
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


/* =========================================================
   IN-GAME LCD INTEGRATION
   ========================================================= */

class $modify(LCDLevelInfoLayer, LevelInfoLayer) {
    struct Fields {
        async::TaskHolder<web::WebResponse> m_request;
        std::optional<RankItemData> m_item;
        CCNode* m_badgeRoot = nullptr;
    };

    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        auto levelId = gdLevelId(level);
        if (levelId <= 0) return true;

        // Show cached info instantly if available, then refresh silently.
        if (auto cached = findCachedRankItem(levelId)) {
            syncProgressFromGD(*cached, level);
            this->showLCDBadge(*cached);
        }

        this->refreshLCDForLevel(levelId);
        return true;
    }

    void clearLCDBadge() {
        if (m_fields->m_badgeRoot) {
            m_fields->m_badgeRoot->removeFromParentAndCleanup(true);
            m_fields->m_badgeRoot = nullptr;
        }
        m_fields->m_item.reset();
    }

    void showLCDBadge(RankItemData const& item) {
        this->clearLCDBadge();
        m_fields->m_item = item;

        auto size = CCDirector::sharedDirector()->getWinSize();
        auto root = CCNode::create();
        root->setContentSize(size);
        root->setAnchorPoint({0.f, 0.f});
        root->setPosition({0.f, 0.f});
        root->setID("lcd-level-info-integration"_spr);
        this->addChild(root, 40);
        m_fields->m_badgeRoot = root;

        auto menu = makeAbsoluteMenu(size);
        root->addChild(menu);

        auto badgeText = rankText(item, true);
        auto badgeSprite = ButtonSprite::create(badgeText.c_str());

        auto badge = CCMenuItemSpriteExtra::create(
            badgeSprite,
            this,
            menu_selector(LCDLevelInfoLayer::onLCDInfo)
        );
        badge->setScale(.52f);
        badge->setID("lcd-rank-badge"_spr);

        auto position = cocos2d::CCPoint{size.width / 2.f, 120.f};
        if (m_difficultySprite && m_difficultySprite->getParent()) {
            // Convert through world space so this remains correct even if another
            // mod / Node IDs nests the difficulty icon under a container.
            auto world = m_difficultySprite->getParent()->convertToWorldSpace(
                m_difficultySprite->getPosition()
            );
            position = this->convertToNodeSpace(world);
            position.y += 43.f;
        }

        badge->setPosition(position);
        menu->addChild(badge);
    }

    void refreshLCDForLevel(int levelId) {
        auto request = web::WebRequest();
        request.timeout(std::chrono::seconds(12));
        request.userAgent("LCD-Geode-Mod/1.5.1");

        m_fields->m_request.spawn(
            "Refresh LCD rank badge",
            request.get(API_URL),
            [this, levelId](web::WebResponse response) {
                if (!response.ok()) return;

                auto jsonResult = response.json();
                if (jsonResult.isErr()) return;

                auto json = jsonResult.unwrap();
                auto parsed = parseItems(json);
                if (!parsed.has_value()) return;

                // Preserve the most recent rank movement even when the user
                // discovers the level from normal GD browsing instead of the LCD list.
                if (auto oldItems = loadCachedRankItems()) {
                    std::unordered_map<std::string, int> oldRanks;
                    for (auto const& oldItem : *oldItems) {
                        oldRanks[snapshotKey(oldItem)] = oldItem.rank;
                    }

                    std::vector<std::string> deltas;
                    for (auto const& newItem : *parsed) {
                        auto it = oldRanks.find(snapshotKey(newItem));
                        if (it != oldRanks.end() && it->second != newItem.rank) {
                            deltas.push_back(fmt::format(
                                "{}\x1e{}",
                                snapshotKey(newItem),
                                it->second - newItem.rank
                            ));
                        }
                    }
                    if (!deltas.empty()) {
                        Mod::get()->setSavedValue("rank-deltas", deltas);
                    }
                }

                Mod::get()->setSavedValue<std::string>(
                    "cache-json",
                    json.dump(matjson::NO_INDENTATION)
                );

                if (!m_level || gdLevelId(m_level) != levelId) return;

                auto found = findRankItemByLevelId(*parsed, levelId);
                if (!found.has_value()) {
                    this->clearLCDBadge();
                    return;
                }

                syncProgressFromGD(*found, m_level);
                this->showLCDBadge(*found);
            }
        );
    }

    void onLCDInfo(CCObject*) {
        if (!m_fields->m_item.has_value()) return;

        if (auto popup = LCDLevelPopup::create(*m_fields->m_item)) {
            popup->show();
        }
    }
};

class $modify(LCDPlayLayer, PlayLayer) {
    void levelComplete() {
        // Save the LCD completion BEFORE GD creates the end screen so the
        // completion screen can immediately show the updated local state.
        if (!m_isPracticeMode && !m_isTestMode && m_level) {
            auto levelId = gdLevelId(m_level);
            if (auto item = findCachedRankItem(levelId)) {
                if (getProgress(*item) < 100) {
                    Mod::get()->setSavedValue<int>(
                        preCompleteProgressKey(*item),
                        getProgress(*item)
                    );
                }
                setProgress(*item, 100);
            }
        }

        PlayLayer::levelComplete();
    }
};

class $modify(LCDPauseLayer, PauseLayer) {
    struct Fields {
        std::optional<RankItemData> m_item;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto play = PlayLayer::get();
        if (!play || !play->m_level) return;

        auto item = findCachedRankItem(gdLevelId(play->m_level));
        if (!item.has_value()) return;

        syncProgressFromGD(*item, play->m_level);
        m_fields->m_item = *item;

        auto size = CCDirector::sharedDirector()->getWinSize();

        // One compact clickable status pill instead of a panel + separate button.
        // This keeps the normal pause UI visually dominant.
        auto menu = makeAbsoluteMenu(size);
        this->addChild(menu, 21);

        auto status = fmt::format(
            "{} | {}%",
            rankText(*item, true),
            getProgress(*item)
        );

        auto button = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(status.c_str()),
            this,
            menu_selector(LCDPauseLayer::onLCDInfo)
        );
        button->setScale(.46f);
        button->setPosition({size.width - 68.f, size.height - 28.f});
        button->setID("lcd-pause-info-button"_spr);
        menu->addChild(button);
    }

    void onLCDInfo(CCObject*) {
        if (!m_fields->m_item.has_value()) return;
        if (auto popup = LCDLevelPopup::create(*m_fields->m_item)) {
            popup->show();
        }
    }
};

class $modify(LCDEndLevelLayer, EndLevelLayer) {
    struct Fields {
        std::optional<RankItemData> m_item;
        std::vector<RankItemData> m_items;
    };

    void customSetup() {
        EndLevelLayer::customSetup();

        if (!m_playLayer || !m_playLayer->m_level) return;
        auto current = findCachedRankItem(gdLevelId(m_playLayer->m_level));
        if (!current.has_value()) return;

        auto all = loadCachedRankItems();
        if (!all.has_value()) return;

        m_fields->m_item = *current;
        m_fields->m_items = *all;

        auto size = CCDirector::sharedDirector()->getWinSize();

        // Small self-contained LCD action card on the upper-right edge.
        // It is intentionally compact so it does not compete with GD's end screen.
        constexpr float CARD_W = 112.f;
        constexpr float CARD_H = 76.f;
        auto cardX = size.width - CARD_W - 12.f;
        auto cardY = size.height - CARD_H - 26.f;

        auto card = CCLayerColor::create(
            ccc4(0, 0, 0, 72),
            CARD_W,
            CARD_H
        );
        card->setPosition({cardX, cardY});
        card->setID("lcd-end-card"_spr);
        this->addChild(card, 29);

        auto header = makeLabel(
            fmt::format("{} COMPLETE", rankText(*current, true)),
            "goldFont.fnt",
            .30f,
            {CARD_W / 2.f, CARD_H - 13.f}
        );
        header->limitLabelWidth(CARD_W - 10.f, .30f, .20f);
        card->addChild(header);

        auto menu = makeAbsoluteMenu(size);
        menu->setID("lcd-end-actions"_spr);
        this->addChild(menu, 30);

        auto centerX = cardX + CARD_W / 2.f;

        auto next = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Next"),
            this,
            menu_selector(LCDEndLevelLayer::onNextLCD)
        );
        next->setScale(.43f);
        next->setPosition({centerX, cardY + 38.f});
        menu->addChild(next);

        auto random = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Random"),
            this,
            menu_selector(LCDEndLevelLayer::onRandomLCD)
        );
        random->setScale(.43f);
        random->setPosition({centerX, cardY + 16.f});
        menu->addChild(random);
    }

    void onNextLCD(CCObject*) {
        if (!m_fields->m_item.has_value()) return;

        auto next = nextUncompletedLCD(
            *m_fields->m_item,
            m_fields->m_items
        );

        if (!next.has_value()) {
            FLAlertLayer::create(
                "LCD",
                "You have no other uncompleted LCD levels in the cached list.",
                "OK"
            )->show();
            return;
        }

        openGDLevel(*next);
    }

    void onRandomLCD(CCObject*) {
        if (!m_fields->m_item.has_value()) return;

        auto random = randomLCD(
            *m_fields->m_item,
            m_fields->m_items
        );
        if (random.has_value()) openGDLevel(*random);
    }
};

class $modify(LCDMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto menu = this->getChildByID("bottom-menu");
        if (!menu) return true;

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
        if (auto popup = LCDListPopup::create()) {
            popup->show();
        }
    }
};
