// SkillEffect Studio - skillplay/Describe.cpp
// Plain-language row descriptions (TR / EN) and the file list of a timeline.
#include "SkillPlay.h"

#include <cwctype>
#include <set>
#include <vector>

namespace se::sp {

namespace {

const wchar_t* PhaseText(Phase p, bool en) {
    switch (p) {
        case Phase::Ready: return en ? L"While preparing" : L"Hazırlanırken";
        case Phase::Wait: return en ? L"While waiting (loop)" : L"Beklerken (döngü)";
        case Phase::Shot: return en ? L"At the attack" : L"Saldırı anında";
        case Phase::ActOS: return en ? L"When the buff starts on party members" : L"Grup üyelerinde güçlendirme başlarken";
        case Phase::ActOL: return en ? L"While the buff lasts on party members" : L"Grup üyelerinde güçlendirme sürdükçe";
        case Phase::ActOE: return en ? L"When the buff ends on party members" : L"Grup üyelerinde güçlendirme biterken";
        case Phase::ActS: return en ? L"When the buff / status starts" : L"Güçlendirme / durum başlarken";
        case Phase::ActL: return en ? L"While the buff / status lasts" : L"Güçlendirme / durum sürdükçe";
        case Phase::Deact: return en ? L"When the buff / status ends" : L"Güçlendirme / durum biterken";
        case Phase::SReturn: return en ? L"On the return (after the projectile)" : L"Dönüşte (mermiden sonra)";
        default: return en ? L"Unknown phase (never plays)" : L"Bilinmeyen faz (hiç oynamaz)";
    }
}

std::wstring Ordinal(int k, bool en) {
    if (!en) return std::to_wstring(k) + L".";
    const wchar_t* suf = (k % 10 == 1 && k % 100 != 11) ? L"st" : (k % 10 == 2 && k % 100 != 12) ? L"nd" : (k % 10 == 3 && k % 100 != 13) ? L"rd" : L"th";
    return std::to_wstring(k) + suf;
}

// a bone name for people: empty = the body ("gövde"), else the bone itself
std::wstring BoneText(const std::wstring& b, bool en, bool target) {
    if (b.empty()) return target ? (en ? L"the target" : L"hedef") : (en ? L"the body" : L"gövde");
    return b;
}

}  // namespace

// plain-language description of one skilleffectset row (rows panel of the object page)
std::wstring DescribeRow(const EffectRow& r, bool en) {
    std::wstring s = PhaseText(r.phase, en);
    if (r.phase == Phase::Ready || r.phase == Phase::Wait || r.phase == Phase::Shot) {
        if (r.startEvent <= 0) s += en ? L", at the start of the animation" : L", animasyonun başında";
        else s += en ? L", at the " + Ordinal(r.startEvent, true) + L" event of the animation" : L", animasyonun " + Ordinal(r.startEvent, false) + L" olayında";
    }
    if (r.movDelay > 0) s += Fmt(en ? L" + %.0f ms" : L" + %.0f ms", r.movDelay);
    s += L": ";
    const std::wstring obj = r.objName;
    const bool none = obj.empty() && r.kind != ObjKind::Wav;
    const std::wstring sb = BoneText(r.startBone, en, false), tb = BoneText(r.targetBone, en, true);
    if (r.kind == ObjKind::Wav) s += (en ? L"sound " : L"ses ") + obj;
    else if (none) s += en ? L"no visible effect (timing / hit row)" : L"görünen efekt yok (zamanlama / vuruş satırı)";
    else s += (r.kind == ObjKind::Bsr ? (en ? L"model " : L"model ") : (en ? L"effect " : L"efekt ")) + obj;
    if (r.Projectile()) {
        const bool mo = r.act == Act::MovOption;
        if (r.act == Act::Target || r.act == Act::TargetF || r.act == Act::DmgPos)
            s += en ? L" falls / moves around the target" : L" hedefin çevresine düşer / orada hareket eder";
        else if (mo)
            s += Fmt(en ? L" flies out from the caster at %.0f°" : L" canavardan %.0f° yönünde dışarı uçar", r.opt.angle);
        else
            s += en ? L" flies from " + sb + L" to " + tb
                    : L", " + (r.startBone.empty() ? std::wstring(L"gövdeden") : sb + L" kemiğinden") + L" " +
                          (r.targetBone.empty() ? std::wstring(L"hedefe") : tb + L" kemiğine") + L" doğru uçar";
        s += Fmt(en ? L" (%.0f-%.0f units/s)" : L" (%.0f-%.0f birim/sn)", r.speedMin, r.speedMax);
        if (!r.objName2.empty()) s += (en ? L", on arrival " : L", varınca ") + r.objName2;
    } else if (!none) {
        switch (r.act) {
            case Act::OneFollow: s += en ? L" follows " + sb : (r.startBone.empty() ? std::wstring(L" gövdeyi izler") : L", " + sb + L" kemiğini izler"); break;
            case Act::Loop: s += en ? L" loops on " + sb + L" until the phase ends" : (r.startBone.empty() ? std::wstring(L" gövdede") : L" " + sb + L" kemiğinde") + L" faz bitene dek döner"; break;
            case Act::Stop: s += en ? L" stays where it appears (" + sb + L")" : L" çıktığı yerde kalır (" + sb + L")"; break;
            case Act::Source: s += en ? L" on the caster" : L" canavarın üstünde"; break;
            case Act::Target: s += en ? L" on " + tb : (r.targetBone.empty() ? std::wstring(L" hedefin üstünde") : L" hedefin " + tb + L" kemiğinde"); break;
            case Act::TargetF: s += en ? L" follows " + tb : (r.targetBone.empty() ? std::wstring(L" hedefi izler") : L" hedefin " + tb + L" kemiğini izler"); break;
            case Act::DmgPos: s += en ? L" where the target is hit" : L" hedefin vurulduğu yerde"; break;
            case Act::Bad: s += en ? L" (ActType unknown: never plays)" : L" (ActType bilinmiyor: hiç oynamaz)"; break;
            default: s += en ? L" at the target (no speed)" : L" hedefte (hız yok)"; break;
        }
    }
    if (r.createCnt > 1) s += Fmt(en ? L", %d copies" : L", %d kopya", r.createCnt);
    if (r.dmg == 1) s += en ? L"; the hit shows here" : L"; vuruş (hasar) burada görünür";
    if (r.dmg < 0) s += en ? L"; DMG Event misspelled (never shows the hit)" : L"; DMG Event yanlış yazılmış (vuruş görünmez)";
    if (r.scale == ScaleBase::Mob) s += en ? L"; grows with the creature size" : L"; canavar boyuyla büyür";
    if (r.script & (kSctShake0 | kSctShake1 | kSctShake2 | kSctShake3 | kSctShakeMov0 | kSctShakeMov1 | kSctShakeMov2 | kSctShakeMov3))
        s += en ? L"; shakes the camera" : L"; kamerayı sarsar";
    if (r.script & kSctMat) s += en ? L"; tints the body" : L"; gövdeyi renklendirir";
    if ((r.script & kSctEffectScale) && !r.scriptArgs.empty()) s += Fmt(en ? L"; effect x%.2f" : L"; efekt x%.2f", r.scriptArgs[0]);
    if (r.trade > 0) s += Fmt(en ? L"; replaces instance %d" : L"; %d nolu örneğin yerini alır", r.trade);
    if (r.kill > 0) s += Fmt(en ? L"; removes instance %d" : L"; %d nolu örneği kaldırır", r.kill);
    if (!r.sndBegin.empty()) s += (en ? L"; sound " : L"; ses ") + r.sndBegin;
    if (!r.sndEnd.empty()) s += (en ? L"; arrival sound " : L"; varış sesi ") + r.sndEnd;
    if (r.origin == Origin::Pending) s += en ? L" [pending: not written yet]" : L" [bekliyor: henüz yazılmadı]";
    return s;
}

// ------------------------------------------------------------------------------- v6 P4: plain-language rows
namespace {

struct PlainWord {
    const wchar_t* key;    // lower-case token of a file name
    const wchar_t* tr;     // Turkish word
    const wchar_t* trHead; // Turkish head form of a compound ("isabet" -> "lanet isabeti"); nullptr = same as tr
    const wchar_t* en;     // English word
};

// words of effect / sound file names; tokens not listed are dropped (creature names, "skill", "mob" ...)
const PlainWord kWords[] = {
    {L"hit", L"isabet", L"isabeti", L"hit"},           {L"curse", L"lanet", nullptr, L"curse"},
    {L"fire", L"ateş", L"ateşi", L"fire"},             {L"flame", L"alev", L"alevi", L"flame"},
    {L"ice", L"buz", L"buzu", L"ice"},                 {L"frost", L"ayaz", L"ayazı", L"frost"},
    {L"cold", L"soğuk", nullptr, L"cold"},             {L"water", L"su", L"suyu", L"water"},
    {L"wind", L"rüzgâr", L"rüzgârı", L"wind"},         {L"earth", L"toprak", L"toprağı", L"earth"},
    {L"stone", L"taş", L"taşı", L"stone"},             {L"rock", L"kaya", L"kayası", L"rock"},
    {L"light", L"ışık", L"ışığı", L"light"},           {L"lightning", L"şimşek", L"şimşeği", L"lightning"},
    {L"thunder", L"gök gürültüsü", nullptr, L"thunder"}, {L"bolt", L"şimşek", L"şimşeği", L"bolt"},
    {L"dark", L"karanlık", L"karanlığı", L"dark"},     {L"shadow", L"gölge", L"gölgesi", L"shadow"},
    {L"holy", L"kutsal", nullptr, L"holy"},            {L"heal", L"iyileştirme", L"iyileştirmesi", L"heal"},
    {L"cure", L"iyileştirme", L"iyileştirmesi", L"cure"}, {L"buff", L"güçlendirme", L"güçlendirmesi", L"buff"},
    {L"debuff", L"zayıflatma", L"zayıflatması", L"debuff"}, {L"poison", L"zehir", L"zehri", L"poison"},
    {L"blood", L"kan", L"kanı", L"blood"},             {L"explosion", L"patlama", L"patlaması", L"explosion"},
    {L"explode", L"patlama", L"patlaması", L"explosion"}, {L"bomb", L"bomba", L"bombası", L"bomb"},
    {L"burst", L"patlama", L"patlaması", L"burst"},    {L"blast", L"patlama", L"patlaması", L"blast"},
    {L"arrow", L"ok", L"oku", L"arrow"},               {L"sword", L"kılıç", L"kılıcı", L"sword"},
    {L"blade", L"bıçak", L"bıçağı", L"blade"},         {L"spear", L"mızrak", L"mızrağı", L"spear"},
    {L"glaive", L"kargı", L"kargısı", L"glaive"},      {L"bow", L"yay", L"yayı", L"bow"},
    {L"staff", L"asa", L"asası", L"staff"},            {L"shield", L"kalkan", L"kalkanı", L"shield"},
    {L"aura", L"aura", L"aurası", L"aura"},            {L"ball", L"küre", L"küresi", L"ball"},
    {L"orb", L"küre", L"küresi", L"orb"},              {L"beam", L"ışın", L"ışını", L"beam"},
    {L"ray", L"ışın", L"ışını", L"ray"},               {L"wave", L"dalga", L"dalgası", L"wave"},
    {L"ring", L"halka", L"halkası", L"ring"},          {L"circle", L"çember", L"çemberi", L"circle"},
    {L"smoke", L"duman", L"dumanı", L"smoke"},         {L"dust", L"toz", L"tozu", L"dust"},
    {L"spark", L"kıvılcım", L"kıvılcımı", L"spark"},   {L"slash", L"kesik", L"kesiği", L"slash"},
    {L"cut", L"kesik", L"kesiği", L"cut"},             {L"stun", L"sersemletme", L"sersemletmesi", L"stun"},
    {L"shock", L"şok", L"şoku", L"shock"},             {L"impact", L"darbe", L"darbesi", L"impact"},
    {L"strike", L"vuruş", L"vuruşu", L"strike"},       {L"attack", L"saldırı", L"saldırısı", L"attack"},
    {L"magic", L"büyü", L"büyüsü", L"magic"},          {L"spell", L"büyü", L"büyüsü", L"spell"},
    {L"cast", L"büyü yapma", L"büyü yapması", L"cast"}, {L"charge", L"güç toplama", L"güç toplaması", L"charge"},
    {L"ready", L"hazırlık", L"hazırlığı", L"ready"},   {L"start", L"başlangıç", L"başlangıcı", L"start"},
    {L"end", L"bitiş", L"bitişi", L"end"},             {L"loop", L"döngü", L"döngüsü", L"loop"},
    {L"trail", L"iz", L"izi", L"trail"},               {L"tail", L"kuyruk", L"kuyruğu", L"tail"},
    {L"ghost", L"hayalet", L"hayaleti", L"ghost"},     {L"spirit", L"ruh", L"ruhu", L"spirit"},
    {L"soul", L"ruh", L"ruhu", L"soul"},               {L"dragon", L"ejderha", L"ejderhası", L"dragon"},
    {L"tiger", L"kaplan", L"kaplanı", L"tiger"},       {L"snake", L"yılan", L"yılanı", L"snake"},
    {L"wolf", L"kurt", L"kurdu", L"wolf"},             {L"bird", L"kuş", L"kuşu", L"bird"},
    {L"flower", L"çiçek", L"çiçeği", L"flower"},       {L"leaf", L"yaprak", L"yaprağı", L"leaf"},
    {L"sun", L"güneş", L"güneşi", L"sun"},             {L"moon", L"ay", L"ayı", L"moon"},
    {L"star", L"yıldız", L"yıldızı", L"star"},         {L"gold", L"altın", L"altını", L"gold"},
    {L"blue", L"mavi", nullptr, L"blue"},              {L"red", L"kırmızı", nullptr, L"red"},
    {L"green", L"yeşil", nullptr, L"green"},           {L"white", L"beyaz", nullptr, L"white"},
    {L"black", L"siyah", nullptr, L"black"},           {L"purple", L"mor", nullptr, L"purple"},
    {L"yellow", L"sarı", nullptr, L"yellow"},          {L"big", L"büyük", nullptr, L"big"},
    {L"small", L"küçük", nullptr, L"small"},           {L"jump", L"zıplama", L"zıplaması", L"jump"},
    {L"fall", L"düşüş", L"düşüşü", L"fall"},           {L"drop", L"damla", L"damlası", L"drop"},
    {L"rain", L"yağmur", L"yağmuru", L"rain"},         {L"storm", L"fırtına", L"fırtınası", L"storm"},
    {L"tornado", L"hortum", L"hortumu", L"tornado"},   {L"recovery", L"iyileşme", L"iyileşmesi", L"recovery"},
    {L"defense", L"savunma", L"savunması", L"defense"}, {L"protect", L"koruma", L"koruması", L"protection"},
    {L"teleport", L"ışınlanma", L"ışınlanması", L"teleport"}, {L"summon", L"çağırma", L"çağırması", L"summon"},
    {L"death", L"ölüm", L"ölümü", L"death"},           {L"die", L"ölüm", L"ölümü", L"death"},
    {L"dead", L"ölüm", L"ölümü", L"death"},            {L"levelup", L"seviye atlama", L"seviye atlaması", L"level up"},
    {L"chain", L"zincir", L"zinciri", L"chain"},       {L"claw", L"pençe", L"pençesi", L"claw"},
    {L"bite", L"ısırık", L"ısırığı", L"bite"},         {L"punch", L"yumruk", L"yumruğu", L"punch"},
    {L"kick", L"tekme", L"tekmesi", L"kick"},          {L"dash", L"atılma", L"atılması", L"dash"},
    {L"rush", L"hücum", L"hücumu", L"rush"},           {L"roar", L"kükreme", L"kükremesi", L"roar"},
    {L"scream", L"çığlık", L"çığlığı", L"scream"},     {L"field", L"alan", L"alanı", L"field"},
    {L"area", L"alan", L"alanı", L"area"},             {L"wall", L"duvar", L"duvarı", L"wall"},
    {L"pillar", L"sütun", L"sütunu", L"pillar"},       {L"crystal", L"kristal", L"kristali", L"crystal"},
    {L"energy", L"enerji", L"enerjisi", L"energy"},    {L"power", L"güç", L"gücü", L"power"},
    {L"force", L"güç", L"gücü", L"force"},             {L"meteor", L"göktaşı", L"göktaşı", L"meteor"},
    {L"fireball", L"ateş topu", L"ateş topu", L"fireball"}, {L"swing", L"savurma", L"savurması", L"swing"},
    {L"step", L"adım", L"adımı", L"step"},             {L"foot", L"ayak", L"ayağı", L"foot"},
    {L"hand", L"el", L"eli", L"hand"},                 {L"head", L"baş", L"başı", L"head"},
    {L"wing", L"kanat", L"kanadı", L"wing"},           {L"horn", L"boynuz", L"boynuzu", L"horn"},
    {L"tooth", L"diş", L"dişi", L"tooth"},             {L"voice", L"ses", L"sesi", L"voice"},
    {L"sound", L"ses", L"sesi", L"sound"},             {L"damage", L"hasar", L"hasarı", L"damage"},
    {L"critical", L"kritik", nullptr, L"critical"},    {L"run", L"koşu", L"koşusu", L"run"},
    {L"walk", L"yürüyüş", L"yürüyüşü", L"walk"},       {L"swim", L"yüzme", L"yüzmesi", L"swim"},
    {L"fly", L"uçuş", L"uçuşu", L"flight"},            {L"heart", L"kalp", L"kalbi", L"heart"},
    {L"cloud", L"bulut", L"bulutu", L"cloud"},         {L"snow", L"kar", L"karı", L"snow"},
    {L"lava", L"lav", L"lavı", L"lava"},               {L"rose", L"gül", L"gülü", L"rose"},
};

std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

const PlainWord* FindWord(const std::wstring& t) {
    for (const auto& w : kWords)
        if (t == w.key) return &w;
    return nullptr;
}

std::wstring PlainNumber(double v) {
    std::wstring s = Fmt(L"%.2f", v);
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s;
}

// "where" of a placed (not flying) effect
std::wstring PlacePlain(const EffectRow& r, bool en) {
    const std::wstring sbx = r.startBone.empty() ? std::wstring() : (en ? L" (at the " : L" (yer: ") + PlainBoneName(r.startBone, en, false) + L")";
    const std::wstring tbx = r.targetBone.empty() ? std::wstring() : (en ? L" (at the " : L" (yer: ") + PlainBoneName(r.targetBone, en, true) + L")";
    switch (r.act) {
        case Act::OneFollow: return en ? L"following the caster" + sbx : L"kullananı izleyen" + sbx;
        case Act::Loop:
            return en ? L"repeating on the caster" + sbx + L" until this stage ends" : L"kullananın üstünde bu aşama bitene dek tekrar eden" + sbx;
        case Act::Stop: return en ? L"staying where it appears" + sbx : L"çıktığı yerde kalan" + sbx;
        case Act::Source: return en ? L"on the caster" + sbx : L"kullananın üstünde" + sbx;
        case Act::Target: return en ? L"on the target" + tbx : L"hedefin üstünde" + tbx;
        case Act::TargetF: return en ? L"following the target" + tbx : L"hedefi izleyen" + tbx;
        case Act::DmgPos: return en ? L"where the target is hit" : L"hedefin vurulduğu yerde";
        case Act::Bad: return en ? L"with an unknown placement (estimated: it does not show)" : L"yeri bilinmeyen (tahmini: görünmez)";
        default: return en ? L"at the target (no speed)" : L"hedefte (hızı yok)";
    }
}

// path of a flying effect: "hedefe düz giden" / "flying straight to the target"
std::wstring FlightPlain(const EffectRow& r, bool en) {
    std::wstring how;
    switch (r.mov) {
        case Mov::Straight: how = en ? L"flying straight" : L"düz giden"; break;
        case Mov::Up: how = en ? L"flying in an upward arc (estimated)" : L"yukarı yay çizerek giden (tahmini)"; break;
        case Mov::Upr: how = en ? L"rising and spreading (estimated)" : L"yükselip dağılarak giden (tahmini)"; break;
        case Mov::Round: how = en ? L"circling (estimated)" : L"dönerek giden (tahmini)"; break;
        case Mov::Hwan: how = en ? L"on a special path (estimated)" : L"özel bir yolla giden (tahmini)"; break;
        default: how = en ? L"flying" : L"giden"; break;
    }
    const std::wstring tb = r.targetBone.empty() ? std::wstring() : (en ? L" (at the " : L" (yer: ") + PlainBoneName(r.targetBone, en, true) + L")";
    switch (r.act) {
        case Act::Target:
        case Act::TargetF:
        case Act::DmgPos: return en ? how + L", falling around the target" : L"hedefin çevresine düşen, " + how;
        case Act::MovOption:
            return en ? how + Fmt(L", spreading out from the caster (%.0f°)", r.opt.angle) : Fmt(L"kullanandan dışarı saçılan (%.0f°), ", r.opt.angle) + how;
        case Act::MovApiece: return en ? how + L" to each target one by one (estimated)" : L"her hedefe ayrı ayrı (tahmini) " + how;
        case Act::MovSplash: return en ? how + L", splashing around the target (estimated)" : L"hedefte çevreye sıçrayan (tahmini), " + how;
        case Act::MovPierce: return en ? how + L", piercing through the target (estimated)" : L"hedefi delip geçen (tahmini), " + how;
        default: return en ? how + L" to the target" + tb : L"hedefe" + tb + L" " + how;
    }
}

}  // namespace

std::wstring PlainBoneName(const std::wstring& bone, bool en, bool target) {
    const std::wstring b = LowerW(Trim(bone));
    if (b.empty() || b == L"*" || b == L"none") return target ? (en ? L"target" : L"hedef") : (en ? L"body" : L"gövde");
    auto has = [&](const wchar_t* k) { return b.find(k) != std::wstring::npos; };
    auto ends = [&](const wchar_t* k) {
        const size_t n = wcslen(k);
        return b.size() >= n && b.compare(b.size() - n, n, k) == 0;
    };
    const bool right = has(L" r ") || has(L"_r_") || has(L" r_") || has(L"right") || ends(L" r") || ends(L"_r");
    const bool left = has(L" l ") || has(L"_l_") || has(L" l_") || has(L"left") || ends(L" l") || ends(L"_l");
    const wchar_t* side = right ? (en ? L"right " : L"sağ ") : left ? (en ? L"left " : L"sol ") : L"";
    if (has(L"footstep")) return en ? L"ground under the feet" : L"ayak altı (yer)";
    struct P {
        const wchar_t* key;
        const wchar_t* tr;
        const wchar_t* en;
    };
    static const P parts[] = {{L"finger", L"parmak", L"finger"}, {L"hand", L"el", L"hand"},           {L"forearm", L"ön kol", L"forearm"},
                              {L"upperarm", L"üst kol", L"upper arm"}, {L"clavicle", L"omuz", L"shoulder"}, {L"head", L"baş", L"head"},
                              {L"neck", L"boyun", L"neck"},      {L"spine", L"sırt", L"back"},          {L"pelvis", L"bel", L"waist"},
                              {L"toe", L"ayak ucu", L"toes"},    {L"foot", L"ayak", L"foot"},           {L"calf", L"baldır", L"calf"},
                              {L"thigh", L"uyluk", L"thigh"},    {L"tail", L"kuyruk", L"tail"},         {L"wing", L"kanat", L"wing"},
                              {L"weapon", L"silah", L"weapon"},  {L"mouth", L"ağız", L"mouth"},         {L"eye", L"göz", L"eye"},
                              {L"jaw", L"çene", L"jaw"},         {L"arm", L"kol", L"arm"},              {L"leg", L"bacak", L"leg"}};
    for (const auto& p : parts)
        if (has(p.key)) return std::wstring(side) + (en ? p.en : p.tr);
    if (b == L"bip01" || b == L"bip") return en ? L"body centre" : L"gövde ortası";
    return Trim(bone) + (en ? L" (bone, estimated)" : L" (kemik, tahmini)");
}

std::wstring PlainFileName(const std::wstring& file, bool en, bool* estimated) {
    std::wstring n = file;
    const size_t sl = n.find_last_of(L"\\/");
    if (sl != std::wstring::npos) n = n.substr(sl + 1);
    const size_t dot = n.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) n = n.substr(0, dot);
    n = LowerW(n);
    std::vector<const PlainWord*> words;
    std::wstring tok;
    auto flush = [&] {
        if (tok.empty()) return;
        const PlainWord* w = FindWord(tok);
        if (!w)  // glued words ("tigerhit"): a known word of 3+ letters at the end of the token
            for (const auto& k : kWords) {
                const size_t kl = wcslen(k.key);
                if (kl >= 3 && tok.size() > kl + 2 && tok.compare(tok.size() - kl, kl, k.key) == 0) {
                    w = &k;
                    break;
                }
            }
        if (w && (words.empty() || words.back() != w)) words.push_back(w);
        tok.clear();
    };
    for (wchar_t c : n) {
        if (iswalpha(c)) tok += c;
        else flush();
    }
    flush();
    if (estimated) *estimated = words.empty();
    if (words.empty()) {
        std::wstring shown = n;
        for (auto& c : shown)
            if (c == L'_') c = L' ';
        return L"\"" + Trim(shown) + L"\"" + (en ? L" (estimated name)" : L" (tahmini ad)");
    }
    std::wstring s;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) s += L' ';
        const bool head = !en && words.size() > 1 && i + 1 == words.size() && words[i]->trHead;
        s += en ? words[i]->en : (head ? words[i]->trHead : words[i]->tr);
    }
    return s;
}

std::wstring DescribeRowPlain(const EffectRow& r, bool en) {
    // when
    std::wstring s;
    const int ev = r.startEvent;
    switch (r.phase) {
        case Phase::Ready:
            s = en ? L"While getting ready" : L"Hazırlanırken";
            if (ev > 0) s += en ? L" (at the " + Ordinal(ev, true) + L" moment of the move)" : L" (hareketin " + Ordinal(ev, false) + L" anında)";
            break;
        case Phase::Wait:
            s = en ? L"While waiting (repeats)" : L"Beklerken (tekrar eder)";
            if (ev > 0) s += en ? L" (at the " + Ordinal(ev, true) + L" moment of the move)" : L" (hareketin " + Ordinal(ev, false) + L" anında)";
            break;
        case Phase::Shot:
            s = ev > 0 ? (en ? L"At the " + Ordinal(ev, true) + L" hit moment" : Ordinal(ev, false) + L" vuruş anında")
                       : (en ? L"As the attack starts" : L"Saldırı başlarken");
            break;
        case Phase::Bad: s = en ? L"At an unknown moment (estimated: it never plays)" : L"Bilinmeyen bir anda (tahmini: hiç oynamaz)"; break;
        default: s = PhaseText(r.phase, en); break;
    }
    if (r.movDelay > 0) s += en ? L", " + PlainNumber(r.movDelay) + L" ms later" : L", " + PlainNumber(r.movDelay) + L" ms sonra";
    // what
    const bool none = r.objName.empty() && r.kind != ObjKind::Wav;
    const std::wstring name = none ? std::wstring() : PlainFileName(r.objName, en);
    if (r.kind == ObjKind::Wav) {
        s += (en ? L", a sound plays: " : L" ses çalar: ") + name;
    } else if (none) {
        s += en ? L": no visible effect (only the timing / the hit moment)" : L": görünen efekt yok (yalnız zamanlama / vuruş anı)";
    } else {
        const wchar_t* noun = r.kind == ObjKind::Bsr ? L"model" : (en ? L"effect" : L"efekt");
        if (r.Projectile()) {
            s += en ? L", a projectile " + FlightPlain(r, true) : L" " + FlightPlain(r, false) + L" mermi";
            if (r.kind == ObjKind::Bsr) s += L" (model)";
        } else {
            s += en ? L", " + std::wstring(noun) + L" " + PlacePlain(r, true) : L" " + PlacePlain(r, false) + L" " + noun;
        }
        s += L": " + name;
    }
    // extras
    if (r.createCnt > 1) s += Fmt(en ? L"; %d copies" : L"; %d tane", r.createCnt);
    if (r.Projectile() && !r.objName2.empty()) s += (en ? L"; on arrival: " : L"; varınca: ") + PlainFileName(r.objName2, en);
    if (r.dmg == 1) s += en ? L"; the damage shows here" : L"; hasar burada görünür";
    if (r.dmg < 0) s += en ? L"; the damage mark is misspelled (estimated: the damage does not show)" : L"; hasar işareti yanlış yazılmış (tahmini: hasar görünmez)";
    if (r.scale == ScaleBase::Mob) s += en ? L"; grows with the creature size" : L"; yaratığın boyuyla büyür";
    if (r.script & (kSctShake0 | kSctShake1 | kSctShake2 | kSctShake3 | kSctShakeMov0 | kSctShakeMov1 | kSctShakeMov2 | kSctShakeMov3))
        s += en ? L"; shakes the screen" : L"; ekranı sarsar";
    if (r.script & kSctMat) s += en ? L"; changes the body colour" : L"; gövdenin rengini değiştirir";
    if ((r.script & kSctEffectScale) && !r.scriptArgs.empty())
        s += en ? L"; effect " + PlainNumber(r.scriptArgs[0]) + L" times as big" : L"; efekt " + PlainNumber(r.scriptArgs[0]) + L" kat büyük";
    if (r.trade > 0) s += en ? L"; takes the place of an earlier effect of this skill" : L"; bu skillin önceki bir efektinin yerini alır";
    if (r.kill > 0) s += en ? L"; ends an earlier effect of this skill" : L"; bu skillin önceki bir efektini kapatır";
    if (!r.sndBegin.empty()) s += (en ? L"; sound: " : L"; ses: ") + PlainFileName(r.sndBegin, en);
    if (!r.sndEnd.empty()) s += (en ? L"; sound on arrival: " : L"; varınca ses: ") + PlainFileName(r.sndEnd, en);
    if (!r.parseNotes.empty()) s += en ? L"; some values could not be read (estimated)" : L"; bazı değerler okunamadı (tahmini)";
    if (r.origin == Origin::Pending) s += en ? L" [waiting: not written yet]" : L" [bekliyor: henüz yazılmadı]";
    return s;
}

std::vector<ResRef> Timeline::Resources() const {
    std::vector<ResRef> out;
    std::set<std::pair<int, std::wstring>> seen;
    auto add = [&](ResArc arc, const std::wstring& p, const std::wstring& what, int row) {
        if (p.empty()) return;
        if (!seen.insert({static_cast<int>(arc), ToUpper(p)}).second) return;
        out.push_back(ResRef{arc, p, what, row});
    };
    for (const auto& a : anims)
        if (!a.file.empty()) add(ResArc::Data, a.file, L"clip " + a.token, -1);
    for (const auto& e : fx) {
        add(e.kind == ObjKind::Bsr ? ResArc::Data : ResArc::Particles, e.res, e.kind == ObjKind::Bsr ? L"model" : L"efp", e.row);
        add(ResArc::Particles, e.arrive, L"ObjName2", e.row);
    }
    for (const auto& h : hits) {
        add(ResArc::Particles, h.damageEfp, L"DamageEfp", h.row);
        add(ResArc::Particles, h.bloodEfp, L"BloodType", h.row);
    }
    for (const auto& s : sounds) add(ResArc::Data, s.path, s.why, -1);
    return out;
}

}  // namespace se::sp
