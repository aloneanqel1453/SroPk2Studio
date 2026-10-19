// bak2 tools - glue over the vendored BAK-2 ddj / core / editor modules (see bak2_tools.hpp).
#include "bak2_tools.hpp"

#include "i18n.hpp"
#include "bak2/core/SkillCrypt.h"
#include "bak2/core/TextDiff.h"
#include "bak2/ddj/DdjCodec.h"
#include "bak2/editor/EdValidate.h"
#include "bak2/glue/TextMap.h"

#include <algorithm>
#include <format>

namespace bak2 {

namespace {

std::wstring FileName(const std::wstring& p) {
    const std::size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

// se::ddj picture readers / DDJ writer reasons (ImageIo.cpp, DdjCodec.cpp): BAK-2 writes them in Turkish only
constexpr glue::TextMap kDdjTexts[] = {
    {L"sıkıştırılmış veri kesik", "b2_ddj_z_truncated"},
    {L"sıkıştırılmamış blok bozuk", "b2_ddj_z_stored_bad"},
    {L"veri beklenenden uzun", "b2_ddj_z_too_long"},
    {L"sıkıştırılmamış blok kesik", "b2_ddj_z_stored_cut"},
    {L"sıkıştırılmış veri bozuk (sabit)", "b2_ddj_z_fixed_bad"},
    {L"sıkıştırma tablosu bozuk", "b2_ddj_z_table_bad"},
    {L"sıkıştırılmış veri bozuk", "b2_ddj_z_bad"},
    {L"bilinmeyen sıkıştırma bloğu", "b2_ddj_z_block_unknown"},
    {L"sıkıştırılmış veri boş", "b2_ddj_z_empty"},
    {L"zlib başlığı bozuk", "b2_ddj_z_header_bad"},
    {L"veri sağlaması (adler32) tutmuyor", "b2_ddj_z_adler"},
    {L"PNG imzası yok", "b2_ddj_png_sig"},
    {L"PNG kesik (parça boyu dosyayı aşıyor)", "b2_ddj_png_cut"},
    {L"PNG bozuk (CRC tutmuyor)", "b2_ddj_png_crc"},
    {L"PNG başlığı kısa", "b2_ddj_png_hdr_short"},
    {L"PNG: desteklenmeyen sıkıştırma / süzgeç / geçmeli kip", "b2_ddj_png_mode"},
    {L"PNG: geçersiz renk türü / bit derinliği", "b2_ddj_png_color"},
    {L"PNG: bilinmeyen zorunlu parça", "b2_ddj_png_chunk"},
    {L"PNG başlığı (IHDR) yok", "b2_ddj_png_ihdr"},
    {L"PNG boyutu desteklenmiyor (en çok 32768 x 32768, 64 M piksel)", "b2_ddj_png_size"},
    {L"PNG: palet (PLTE) yok", "b2_ddj_png_plte"},
    {L"PNG piksel verisi kesik", "b2_ddj_png_pixels"},
    {L"PNG: bilinmeyen satır süzgeci", "b2_ddj_png_filter"},
    {L"BMP imzası yok", "b2_ddj_bmp_sig"},
    {L"BMP başlığı bozuk", "b2_ddj_bmp_hdr"},
    {L"BMP renk maskeleri kesik", "b2_ddj_bmp_masks"},
    {L"BMP boyutu desteklenmiyor", "b2_ddj_bmp_size"},
    {L"BMP içinde JPEG / PNG var (desteklenmiyor)", "b2_ddj_bmp_embedded"},
    {L"BMP sıkıştırması desteklenmiyor", "b2_ddj_bmp_comp"},
    {L"BMP bit derinliği desteklenmiyor", "b2_ddj_bmp_bits"},
    {L"BMP piksel verisi yok", "b2_ddj_bmp_nopix"},
    {L"BMP RLE bit derinliği uyumsuz", "b2_ddj_bmp_rle"},
    {L"BMP piksel verisi kesik", "b2_ddj_bmp_cut"},
    {L"TGA dosyası kısa", "b2_ddj_tga_short"},
    {L"TGA türü desteklenmiyor", "b2_ddj_tga_type"},
    {L"TGA boyutu geçersiz", "b2_ddj_tga_size"},
    {L"TGA bit derinliği desteklenmiyor", "b2_ddj_tga_bits"},
    {L"TGA renk tablosu bozuk", "b2_ddj_tga_cmap_bad"},
    {L"TGA renk tablosu yok", "b2_ddj_tga_cmap_none"},
    {L"TGA piksel verisi kesik", "b2_ddj_tga_cut"},
    {L"TGA RLE verisi kesik", "b2_ddj_tga_rle"},
    {L"resim boş ya da çok büyük", "b2_ddj_img_empty_big"},
    {L"resim biçimi tanınmadı (PNG, BMP, TGA, JPG, DDS, DDJ desteklenir)", "b2_ddj_img_format_list"},
    {L"DDS / DDJ çözülemedi: {}", "b2_ddj_dds_decode"},
    {L"resim biçimi tanınmadı", "b2_ddj_img_format"},
    {L"dosya boş", "b2_ddj_file_empty"},
    {L"DDS başlığı yok ya da kısa", "b2_ddj_dds_hdr"},
    {L"DDS boyutu geçersiz", "b2_ddj_dds_size"},
    {L"resim boş", "b2_ddj_img_empty"},
    {L"resim çok büyük (en çok 16384 x 16384)", "b2_ddj_img_big"},
    {L"bilinmeyen doku biçimi", "b2_ddj_fmt_unknown"},
    {L"mip seviyesi sayısı resim boyutunu aşıyor", "b2_ddj_mips_count"},
    {L"mip seviyesi boyutu yanlış (seviye {})", "b2_ddj_mip_size"},
    {L"iptal edildi", "b2_ddj_cancelled"},
};

// a se::ddj reason in the UI language; fallback = the bridge's own sentence when BAK-2 gave none
std::wstring DdjText(const std::wstring& why, std::string_view fallback) {
    return why.empty() ? pk2::L(fallback) : glue::Localize(why, kDdjTexts);
}

// se::ed::KindName (EdSchema.cpp, TR / EN): the kinds with words in their name; the rest are table names
std::wstring KindText(se::ed::TKind k) {
    using K = se::ed::TKind;
    switch (k) {
        case K::CharacterData: return pk2::L("b2_kind_chardata");
        case K::ItemData: return pk2::L("b2_kind_itemdata");
        case K::SkillData: return pk2::L("b2_kind_skilldata");
        case K::NameTable: return pk2::L("b2_kind_names");
        case K::IndexFile: return pk2::L("b2_kind_index");
        case K::Generic:
        case K::Unknown: return pk2::L("b2_kind_generic");
        default: return se::ed::KindName(k, true);
    }
}

// se::ed::SevName (EdValidate.cpp, TR / EN)
std::wstring SevText(se::ed::Sev s) {
    switch (s) {
        case se::ed::Sev::Error: return pk2::L("error");
        case se::ed::Sev::Warn: return pk2::L("b2_sev_warn");
        default: return pk2::L("b2_sev_info");
    }
}

}  // namespace

bool decode_image_bgra(const std::vector<std::uint8_t>& bytes, int& w, int& h, std::vector<std::uint8_t>& bgra,
                       std::wstring* err) {
    se::ddj::Image img;
    std::wstring why;
    if (bytes.empty() || !se::ddj::DecodePicture(bytes.data(), bytes.size(), img, &why) || img.Empty()) {
        if (err) *err = DdjText(why, "b2_ddj_decode_fail");
        return false;
    }
    w = img.w;
    h = img.h;
    bgra.resize(img.rgba.size());
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        bgra[i] = img.rgba[i + 2];
        bgra[i + 1] = img.rgba[i + 1];
        bgra[i + 2] = img.rgba[i];
        bgra[i + 3] = img.rgba[i + 3];
    }
    return true;
}

bool encode_ddj(const std::vector<std::uint8_t>& imageFile, const std::vector<std::uint8_t>* oldDdj,
                std::vector<std::uint8_t>& outDdj, std::wstring& err) {
    err.clear();
    outDdj.clear();
    se::ddj::Image img;
    std::wstring why;
    if (imageFile.empty() || !se::ddj::DecodePicture(imageFile.data(), imageFile.size(), img, &why) || img.Empty()) {
        err = DdjText(why, "b2_ddj_decode_fail");
        return false;
    }
    se::ddj::DdjOptions opt;   // new file: Auto (DXT1 / DXT5), no mips, shipped header
    if (oldDdj && !oldDdj->empty()) {
        se::ddj::DdjInfo info;
        if (!se::ddj::ReadDdjInfo(oldDdj->data(), oldDdj->size(), info, &why)) {
            err = pk2::Lf("b2_ddj_old_unreadable", glue::Localize(why, kDdjTexts));
            return false;
        }
        opt = se::ddj::OptionsLike(info, oldDdj->data(), oldDdj->size());
    }
    if (!se::ddj::EncodeDdj(img, opt, outDdj, &why)) {
        err = DdjText(why, "b2_ddj_write_fail");
        outDdj.clear();
        return false;
    }
    return true;
}

bool is_skill_enc(const std::vector<std::uint8_t>& bytes) { return se::IsSkillEnc(bytes); }

bool skill_decrypt(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) {
    out.clear();
    return se::SkillDecrypt(in, out);
}

std::vector<std::uint8_t> skill_encrypt(const std::vector<std::uint8_t>& plain) { return se::SkillEncrypt(plain); }

std::wstring text_diff(const std::wstring& a, const std::wstring& b) {
    const se::LineDiff d = se::DiffText(a, b);
    if (a == b) return pk2::L("b2_diff_none") + L"\n";
    if (!d.counts.Any()) return pk2::L("b2_diff_eol") + L"\n";
    std::wstring s = pk2::Lf("b2_diff_counts", d.counts.added, d.counts.changed, d.counts.removed) +
                     (d.exact ? std::wstring() : pk2::L("b2_diff_approx")) + L"\n";
    for (const auto& hk : se::DiffHunks(d.ops, 3)) {
        int la = 0, lb = 0;
        for (std::size_t k = hk.op0; k < hk.op1; ++k) {
            if (d.ops[k].a >= 0 && !la) la = d.ops[k].a + 1;
            if (d.ops[k].b >= 0 && !lb) lb = d.ops[k].b + 1;
        }
        s += pk2::Lf("b2_diff_hunk", std::max(la, 1), std::max(lb, 1)) + L"\n";
        for (std::size_t k = hk.op0; k < hk.op1; ++k) {
            const se::DiffOp& o = d.ops[k];
            const std::wstring& t =
                o.kind == se::DiffKind::Add ? d.b[static_cast<std::size_t>(o.b)] : d.a[static_cast<std::size_t>(o.a)];
            s += o.kind == se::DiffKind::Same ? L"  " : o.kind == se::DiffKind::Add ? L"+ " : L"- ";
            s += t;
            s += L'\n';
        }
    }
    return s;
}

bool table_validate(const std::wstring& innerPath, const std::vector<std::uint8_t>& bytes, TableCheck& out) {
    out = TableCheck{};
    const std::wstring name = FileName(innerPath);
    se::ed::EdDoc doc;
    std::wstring err;
    if (!doc.Load(name.empty() ? L"table.txt" : name, bytes, err)) {
        out.errors = 1;
        out.report = pk2::Lf("b2_table_unreadable", err) + L"\n";
        return false;
    }
    out.rows = static_cast<int>(doc.Visible().size());
    out.roundtrip = doc.Save() == bytes;
    const std::vector<se::ed::CellIssue> issues = se::ed::CheckDoc(doc, 0, nullptr);
    for (const auto& i : issues) {
        if (i.sev == se::ed::Sev::Error) ++out.errors;
        else if (i.sev == se::ed::Sev::Warn) ++out.warnings;
        else ++out.infos;
    }
    out.report = pk2::Lf("b2_tv_header", name, KindText(doc.kind), std::wstring(doc.enc ? L" (ENC)" : L""), out.rows, out.errors,
                         out.warnings, out.infos, out.roundtrip ? pk2::L("b2_tv_exact") : pk2::L("b2_tv_differs")) +
                 L"\n";
    constexpr std::size_t kMaxLines = 2000;
    std::size_t shown = 0;
    for (const auto& i : issues) {
        if (shown++ == kMaxLines) {
            out.report += pk2::Lf("b2_tv_more", issues.size() - kMaxLines) + L"\n";
            break;
        }
        std::wstring where = i.line >= 0 ? pk2::Lf("b2_tv_line", i.line + 1) : pk2::L("b2_tv_file");
        if (i.col >= 0) {
            std::wstring col;
            if (const se::ed::EdRow* r = doc.Row(i.uid))
                if (const se::ed::ColDef* c = doc.SchemaOf(*r).Col(static_cast<std::size_t>(i.col))) col = c->name;
            where += L" " + pk2::Lf("b2_tv_col", i.col + 1);
            if (!col.empty()) where += L" (" + col + L")";
        }
        // the finding itself: BAK-2 has it in Turkish and English (EdValidate CheckDoc)
        out.report += std::format(L"{} [{}] {}: {}\n", where, SevText(i.sev), i.code, glue::Bak2English() ? i.msgEn : i.msgTr);
    }
    return true;
}

bool table_roundtrip_ok(const std::vector<std::uint8_t>& bytes, const std::wstring& innerPath) {
    const std::wstring name = FileName(innerPath);
    se::ed::EdDoc doc;
    std::wstring err;
    return doc.Load(name.empty() ? L"table.txt" : name, bytes, err) && doc.Save() == bytes;
}

}  // namespace bak2
