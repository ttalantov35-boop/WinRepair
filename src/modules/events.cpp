// Event log access via wevtapi (loaded dynamically: the exe starts even if the DLL is absent), BSOD report.
#include "../win/app.h"
#include "../core/bsod_catalog.h"

#include <winevt.h>

namespace wr {

namespace {

struct EvtApi {
    HMODULE h = nullptr;
    decltype(&EvtQuery) Query = nullptr;
    decltype(&EvtNext) Next = nullptr;
    decltype(&EvtRender) Render = nullptr;
    decltype(&EvtClose) Close = nullptr;
    decltype(&EvtOpenPublisherMetadata) OpenPub = nullptr;
    decltype(&EvtFormatMessage) Fmt = nullptr;
    decltype(&EvtCreateRenderContext) CreateCtx = nullptr;
    bool ok() const { return Query && Next && Render && Close && CreateCtx; }
};

EvtApi& Api() {
    static EvtApi a;
    static bool init = false;
    if (!init) {
        init = true;
        a.h = LoadLibraryW(L"wevtapi.dll");
        if (a.h) {
            a.Query = (decltype(&EvtQuery))GetProcAddress(a.h, "EvtQuery");
            a.Next = (decltype(&EvtNext))GetProcAddress(a.h, "EvtNext");
            a.Render = (decltype(&EvtRender))GetProcAddress(a.h, "EvtRender");
            a.Close = (decltype(&EvtClose))GetProcAddress(a.h, "EvtClose");
            a.OpenPub = (decltype(&EvtOpenPublisherMetadata))GetProcAddress(a.h, "EvtOpenPublisherMetadata");
            a.Fmt = (decltype(&EvtFormatMessage))GetProcAddress(a.h, "EvtFormatMessage");
            a.CreateCtx = (decltype(&EvtCreateRenderContext))GetProcAddress(a.h, "EvtCreateRenderContext");
        }
    }
    return a;
}

const char* LevelName(unsigned l) {
    switch (l) {
        case 1: return "Критический";
        case 2: return "Ошибка";
        case 3: return "Предупреждение";
        case 4: return "Сведения";
        case 5: return "Подробно";
        default: return "—";
    }
}

}  // namespace

std::string XpathRecent(int days, const std::string& extraCond) {
    long long ms = (long long)days * 86400000LL;
    std::string q = "*[System[";
    if (!extraCond.empty()) q += extraCond + " and ";
    q += fmt("TimeCreated[timediff(@SystemTime) <= %lld]]]", ms);
    return q;
}

bool QueryEvents(const Ctx& c, const std::string& logName, const std::string& xpath, size_t maxRows, bool wantMsg, bool wantXml,
                 std::vector<EvtRow>& out, std::string& err) {
    EvtApi& api = Api();
    if (!api.ok()) { err = "wevtapi.dll недоступна в этой среде"; return false; }
    DWORD flags = EvtQueryReverseDirection | EvtQueryTolerateQueryErrors;
    std::wstring path;
    if (c.target.online) {
        flags |= EvtQueryChannelPath;
        path = W(logName);
    } else {
        flags |= EvtQueryFilePath;
        std::string f = c.target.winDir + "\\System32\\winevt\\Logs\\" + logName + ".evtx";
        if (!PathExists(f)) { err = "Файл журнала не найден: " + f; return false; }
        path = W(f);
    }
    EVT_HANDLE q = api.Query(nullptr, path.c_str(), W(xpath).c_str(), flags);
    if (!q) { err = "EvtQuery: " + ErrMsg(GetLastError()); return false; }
    EVT_HANDLE ctx = api.CreateCtx(0, nullptr, EvtRenderContextSystem);
    std::map<std::wstring, EVT_HANDLE> pubs;
    std::vector<uint8_t> buf(8192);
    bool more = true;
    while (more && out.size() < maxRows && !c.cancelled()) {
        EVT_HANDLE ev[32];
        DWORD got = 0;
        if (!api.Next(q, 32, ev, 2000, 0, &got)) {
            DWORD le = GetLastError();
            if (le != ERROR_NO_MORE_ITEMS && le != ERROR_TIMEOUT) err = "EvtNext: " + ErrMsg(le);
            break;
        }
        for (DWORD i = 0; i < got; ++i) {
            if (out.size() < maxRows) {
                EvtRow row;
                row.log = logName;
                DWORD used = 0, cnt = 0;
                if (!api.Render(ctx, ev[i], EvtRenderEventValues, (DWORD)buf.size(), buf.data(), &used, &cnt) && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                    buf.resize(used);
                    api.Render(ctx, ev[i], EvtRenderEventValues, (DWORD)buf.size(), buf.data(), &used, &cnt);
                }
                std::wstring wprov;
                if (cnt > EvtSystemTimeCreated) {
                    auto* v = (EVT_VARIANT*)buf.data();
                    if ((v[EvtSystemProviderName].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeString && v[EvtSystemProviderName].StringVal) {
                        wprov = v[EvtSystemProviderName].StringVal;
                        row.provider = U(wprov);
                    }
                    if ((v[EvtSystemEventID].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeUInt16) row.id = v[EvtSystemEventID].UInt16Val;
                    if ((v[EvtSystemLevel].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeByte) row.level = LevelName(v[EvtSystemLevel].ByteVal);
                    if ((v[EvtSystemTimeCreated].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeFileTime) row.time = FileTimeStr(v[EvtSystemTimeCreated].FileTimeVal);
                }
                if (wantMsg && api.OpenPub && api.Fmt && !wprov.empty()) {
                    auto it = pubs.find(wprov);
                    EVT_HANDLE pub;
                    if (it == pubs.end()) { pub = api.OpenPub(nullptr, wprov.c_str(), nullptr, 0, 0); pubs[wprov] = pub; }
                    else pub = it->second;
                    if (pub) {
                        DWORD need = 0;
                        api.Fmt(pub, ev[i], 0, 0, nullptr, EvtFormatMessageEvent, 0, nullptr, &need);
                        if (need > 0 && need < (1u << 20)) {
                            std::wstring m(need, L'\0');
                            DWORD u2 = 0;
                            if (api.Fmt(pub, ev[i], 0, 0, nullptr, EvtFormatMessageEvent, need, &m[0], &u2)) {
                                std::string s = U(m.c_str());
                                for (auto& ch : s) if (ch == '\r' || ch == '\n') ch = ' ';
                                row.message = truncUtf8(s, 300);
                            }
                        }
                    }
                }
                if (wantXml) {
                    DWORD need = 0, cnt2 = 0;
                    api.Render(nullptr, ev[i], EvtRenderEventXml, 0, nullptr, &need, &cnt2);
                    if (need > 0 && need < (4u << 20)) {
                        std::wstring x(need / 2 + 1, L'\0');
                        if (api.Render(nullptr, ev[i], EvtRenderEventXml, need, &x[0], &need, &cnt2)) row.xml = U(x.c_str());
                    }
                }
                out.push_back(std::move(row));
            }
            api.Close(ev[i]);
        }
    }
    for (auto& p : pubs) if (p.second) api.Close(p.second);
    api.Close(ctx);
    api.Close(q);
    return err.empty();
}

// ---------------------------------------------------------------------------------------------

void ModEvents(Ctx& c) {
    c.info(fmt("=== События Critical/Error за %d дн. ===", c.opt.days));
    if (!c.needTarget()) return;
    std::vector<EvtRow> all;
    for (const char* lg : {"System", "Application"}) {
        std::vector<EvtRow> rows;
        std::string err;
        std::string xp = XpathRecent(c.opt.days, "(Level=1 or Level=2)");
        if (!QueryEvents(c, lg, xp, 3000, true, false, rows, err)) c.warn(fmt("Журнал %s: %s", lg, err.c_str()));
        c.info(fmt("Журнал %s: %zu событий", lg, rows.size()));
        all.insert(all.end(), rows.begin(), rows.end());
        if (c.cancelled()) return;
    }
    std::sort(all.begin(), all.end(), [](const EvtRow& a, const EvtRow& b) { return a.time > b.time; });
    Section sec;
    sec.title = "События";
    if (all.empty()) {
        c.ok(fmt("Событий Critical/Error за %d дн. нет.", c.opt.days));
        sec.rows.push_back({"Critical/Error", "0"});
    } else {
        std::string csv = "LogName,Time,Id,Level,Provider,Message\r\n";
        std::map<std::string, int> byProv;
        for (const auto& e : all) {
            csv += csvQuote(e.log) + "," + csvQuote(e.time) + "," + std::to_string(e.id) + "," + csvQuote(e.level) + "," + csvQuote(e.provider) + "," + csvQuote(e.message) + "\r\n";
            byProv[e.provider]++;
        }
        std::vector<std::pair<int, std::string>> top;
        for (auto& p : byProv) top.push_back({p.second, p.first});
        std::sort(top.begin(), top.end(), [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) { return a.first > b.first; });
        std::string txt = "Count  Provider\r\n";
        c.info(fmt("Всего событий: %zu. Топ источников:", all.size()));
        for (size_t i = 0; i < top.size(); ++i) {
            txt += fmt("%5d  %s\r\n", top[i].first, top[i].second.c_str());
            if (i < 8) c.info(fmt("  %5d  %s", top[i].first, top[i].second.c_str()));
        }
        for (size_t i = 0; i < all.size() && i < 12; ++i)
            c.info(fmt("  %s [%s] %s #%u: %s", all[i].time.c_str(), all[i].log.c_str(), all[i].provider.c_str(), all[i].id, truncUtf8(all[i].message, 110).c_str()));
        c.saveText("events.csv", csv);
        c.saveText("by-provider.txt", txt);
        sec.rows.push_back({"Critical/Error", std::to_string(all.size())});
        for (size_t i = 0; i < top.size() && i < 5; ++i) sec.rows.push_back({top[i].second, std::to_string(top[i].first)});
    }
    if (c.res) c.res->setSection(sec);
}

// ---------------------------------------------------------------------------------------------

void ModBsod(Ctx& c) {
    int days = c.opt.days < 30 ? 30 : c.opt.days;  // original default window for BSOD report: 30 days
    c.info(fmt("=== BSOD: события BugCheck и дампы (%d дн.) ===", days));
    if (!c.needTarget()) return;
    Json rep = Json::object();
    rep.set("Timestamp", IsoNow()).set("Days", days);
    Section sec;
    sec.title = "BSOD";

    std::vector<EvtRow> rows;
    std::string err;
    std::string cond = "Provider[@Name='Microsoft-Windows-WER-SystemErrorReporting'] and EventID=1001";
    if (!QueryEvents(c, "System", XpathRecent(days, cond), 200, false, true, rows, err)) c.warn("События BugCheck: " + err);
    Json je = Json::array();
    for (const auto& r : rows) {
        uint32_t code = 0;
        bool have = false;
        size_t p = r.xml.find("Name=\"param1\">");
        if (p != std::string::npos) have = parseHexCode(r.xml.substr(p, 120), code);
        const BsodEntry* ce = have ? bsodByCode(code) : nullptr;
        std::string line = fmt("%s  BugCheck %s %s", r.time.c_str(), have ? fmt("0x%08X", code).c_str() : "?", ce ? ce->name : "(нет в каталоге)");
        c.warn(line);
        Json o = Json::object();
        o.set("Time", r.time).set("Code", have ? fmt("0x%08X", code) : "").set("CodeName", ce ? ce->name : "");
        if (ce) {
            o.set("Catalog", Json::object().set("category", ce->category).set("first_step", ce->firstStep).set("note", ce->note));
            c.info(fmt("   категория: %s; с чего начать: %s; примечание: %s", ce->category, ce->firstStep, ce->note));
        }
        je.add(o);
        sec.rows.push_back({r.time, fmt("%s %s", have ? fmt("0x%08X", code).c_str() : "?", ce ? ce->name : "")});
    }
    if (rows.empty() && err.empty()) c.ok("Событий BugCheck за период нет.");
    rep.set("Events", je);

    // dumps
    std::string wd = DumpDirOf(c.target);
    Json jd = Json::array();
    std::vector<std::pair<std::string, std::string>> dumpFiles;
    for (const auto& f : ListFiles(wd + "\\Minidump\\", "*.dmp")) dumpFiles.push_back({wd + "\\Minidump\\" + f, f});
    if (PathExists(wd + "\\MEMORY.DMP")) dumpFiles.push_back({wd + "\\MEMORY.DMP", "MEMORY.DMP"});
    for (const auto& d : dumpFiles) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(W(d.first).c_str(), GetFileExInfoStandard, &fa);
        unsigned long long sz = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        unsigned long long ft = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
        c.info(fmt("Дамп: %s  %s  %s", d.first.c_str(), fmtBytes(sz).c_str(), FileTimeStr(ft).c_str()));
        Json o = Json::object();
        o.set("Path", d.first).set("SizeBytes", sz).set("Modified", FileTimeStr(ft));
        if (sz < 65536) { o.set("SizeWarning", "Очень мал — возможен обрезанный"); c.warn("  Дамп очень мал — возможен обрезанный."); }
        jd.add(o);
    }
    if (dumpFiles.empty()) c.info("Файлы дампов не найдены (" + wd + "\\Minidump, MEMORY.DMP).");
    sec.rows.push_back({"Дампов", std::to_string(dumpFiles.size())});
    rep.set("Dumps", jd);
    rep.set("Recommendation", Json::array().add("Код BSOD — категория, не диагноз.").add("Разбор: кнопка «Разбор дампов» (быстрый разбор заголовка + cdb !analyze -v).")
                                  .add("Проверьте: целостность, память, диски, драйверы."));
    c.info("Код BSOD — категория, не диагноз. Для причины используйте «Разбор дампов».");
    c.saveText("bsod.json", rep.dump());
    if (c.res) { c.res->haveBsod = true; c.res->bsodEvents = (int)rows.size(); c.res->setSection(sec); }
}

}  // namespace wr
