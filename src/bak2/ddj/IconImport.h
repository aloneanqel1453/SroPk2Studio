// SkillEffect Studio v6 - ddj/IconImport.h  (M33)
// Icon import: PNG / BMP / TGA / JPG / DDS picture -> game icon .ddj with the size + layout the client folder
// uses (rules measured from the shipped Media icons, learnable from any client), the decoded result (what the
// game will show) and a preview sheet. The textdata column takes the Media path without the leading "icon\":
// skilldata column 61 UI_IconFile, characterdata / itemdata column 54 AssocFileIcon128 (+ the table's offset).
// No client / GUI types here; the client adapter (DdjClient.h) learns rules from a Vfs.
#pragma once

#include "DdjCodec.h"

#include <string>
#include <vector>

namespace se::ddj {

struct IconRule {
    std::wstring prefix;            // Media folder, lower case, ends with '\' ("icon\skill\")
    int w = 32, h = 32;
    DdjFormat format = DdjFormat::Argb8888;
    int samples = 0;                // shipped files behind the rule (0 = built-in default)
    std::wstring altTr;             // other common layout in that folder ("A1R5G5B5 %40")
};

class IconRules {
public:
    static IconRules Defaults();    // CLEAN-CLIENT measurements (4 489 icons)
    const IconRule& Match(std::wstring_view mediaPath) const;   // longest prefix, else the generic 32 x 32 rule
    std::vector<IconRule>& Rules() { return m_rules; }
    const std::vector<IconRule>& Rules() const { return m_rules; }
    std::wstring Describe(bool english = false) const;           // one line per rule
    bool learned = false;

private:
    std::vector<IconRule> m_rules;
    IconRule m_generic;
};

// collects shipped icon headers (path + DdjInfo) and builds rules for every folder at depth 2 and 3
class IconRuleLearner {
public:
    void Add(std::wstring_view mediaPath, const DdjInfo& info);
    IconRules Build(int minSamples = 3) const;   // starts from Defaults(), learned folders override
    int Count() const { return m_count; }

private:
    struct Key { int w, h; DdjFormat f; bool operator<(const Key& o) const { return w != o.w ? w < o.w : h != o.h ? h < o.h : f < o.f; } };
    std::vector<std::pair<std::wstring, std::vector<std::pair<Key, int>>>> m_dirs;
    int m_count = 0;
};

enum class IconKind : int { Skill = 0, Item, Item64, SkillGroup, SkillMastery, Action, Other };

struct IconImportOptions {
    int w = 0, h = 0;                       // 0 = from the rule of the target path
    DdjFormat format = DdjFormat::Auto;     // Auto = the rule's format
    Fit fit = Fit::Contain;
    Filter filter = Filter::Mitchell;
    bool trim = true;                       // crop a transparent border before fitting
    float sharpen = 0.0f;                   // 0..1 unsharp mask after shrinking (0 = off)
    std::uint32_t background = 0xFF000000;  // RGBA (R low byte) under transparent pixels for formats without alpha
    DxtQuality quality = DxtQuality::Best;
};

struct IconImportResult {
    Bytes ddj;                      // ready .ddj (JMXVDDJ + DDS, shipped header layout)
    Image fitted;                   // the w x h picture before encoding
    Image game;                     // decoded back from ddj = what the client shows
    Image preview;                  // sheet: source, slot frame 1x, 2x, 4x
    int w = 0, h = 0;
    DdjFormat format = DdjFormat::Argb8888;
    IconRule rule;
    std::wstring columnValue;       // textdata value (Media path without "icon\")
    double psnr = 99;               // fitted vs game (RGB + alpha)
    std::vector<std::wstring> notesTr;
};

// targetMediaPath: "icon\skill\custom\my_skill.ddj" (may be empty -> opts.w/h or 32 x 32). frame: optional slot
// frame picture for the preview (e.g. decoded interface\...\slot .ddj); nullptr = drawn frame.
bool ImportIcon(const Image& src, const std::wstring& targetMediaPath, const IconRules& rules, const IconImportOptions& opts,
                IconImportResult& out, std::wstring* whyTr = nullptr, const Image* frame = nullptr);

// client path rules: ASCII only (the client reads CP949; Turkish letters break the lookup), \ separators,
// <= 255 chars, .ddj extension. whyTr names the first bad character.
bool CheckIconPath(const std::wstring& mediaPath, std::wstring* whyTr = nullptr);
std::wstring SuggestIconPath(IconKind kind, const std::wstring& code, bool europe = false);
std::wstring IconColumnValue(const std::wstring& mediaPath);   // strips "icon\"
const wchar_t* IconColumnName(IconKind kind);                  // "UI_IconFile" / "AssocFileIcon128"
int IconColumnIndex(IconKind kind);                            // 61 skilldata, 54 characterdata / itemdata

Image PreviewSheet(const Image& source, const Image& game, const Image* frame);

}  // namespace se::ddj
