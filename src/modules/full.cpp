// Full diagnostic (9 modules) + health score + HTML report (port of Invoke-WRFullDiagnostic / Export-WRHtmlReport).
#include "../win/app.h"
#include "../core/health.h"

namespace wr {

std::string BuildHtmlReport(const Ctx& c, const Results& r, int score, const std::string& band, const std::vector<std::string>& reasons) {
    const Env& e = GetEnv();
    std::string h;
    h += "<!DOCTYPE html><html lang=\"" + std::string(CurrentLang() == Lang::EN ? "en" : "ru") + "\"><head><meta charset=\"utf-8\"><title>WinRepair " + htmlEscape(NiceNow()) + "</title><style>"
         "body{font-family:Segoe UI,Arial,sans-serif;margin:24px;color:#1a1a1a;background:#fafafa}h1{color:#0a4d8c}"
         "h2{color:#0a4d8c;border-bottom:2px solid #0a4d8c;padding-bottom:4px;margin-top:32px}"
         "table{border-collapse:collapse;width:100%;margin:8px 0;background:#fff}th,td{border:1px solid #ddd;padding:6px 10px;text-align:left;font-size:14px}"
         "th{background:#eef4fa;width:30%}.score{font-size:42px;font-weight:bold;padding:16px;border-radius:8px;display:inline-block}"
         ".Excellent{background:#d4edda;color:#155724}.Good{background:#d1ecf1;color:#0c5460}.Fair{background:#fff3cd;color:#856404}"
         ".Poor{background:#f8d7da;color:#721c24}.Critical{background:#721c24;color:#fff}.note{color:#666;font-size:12px;font-style:italic}"
         "</style></head><body>";
    h += "<h1>" + Tr("WinRepair — отчёт") + "</h1><p>" + htmlEscape(NiceNow()) + " · " + htmlEscape(e.computer) + " · " + htmlEscape(c.target.label) + "</p>";
    h += fmt(("<h2>" + Tr("Оценка") + "</h2><div class='score %s'>%d / 100 &nbsp; %s</div>").c_str(), band.c_str(), score, band.c_str());
    if (!reasons.empty()) {
        h += "<ul>";
        for (const auto& s : reasons) h += "<li>" + htmlEscape(Tr(s)) + "</li>";
        h += "</ul>";
    }
    h += "<p class='note'>Синтетическая оценка для клиента. Не заменяет решение инженера.</p>";
    for (const auto& s : r.sections) {
        h += "<h2>" + htmlEscape(Tr(s.title)) + "</h2><table>";
        for (const auto& row : s.rows) h += "<tr><th>" + htmlEscape(Tr(row.first)) + "</th><td>" + htmlEscape(Tr(row.second)) + "</td></tr>";
        h += "</table>";
    }
    h += "<p class='note'>Сформировано " + std::string(kAppName) + " " + kAppVersion + "</p></body></html>";
    return h;
}

void ModFull(Ctx& c) {
    c.info("=== ПОЛНАЯ ДИАГНОСТИКА (9 модулей + оценка + HTML) ===");
    if (!c.res) { c.error("Внутренняя ошибка: нет контекста результатов."); return; }
    *c.res = Results();  // fresh results for this run
    struct Step { const char* name; const char* dir; ModuleFn fn; };
    const Step steps[] = {
        {"1/9 Система", "system", ModSystem},   {"2/9 События", "events", ModEvents},     {"3/9 Целостность", "integrity", ModIntegrity},
        {"4/9 Драйверы", "drivers", ModDrivers}, {"5/9 Диски", "disk", ModDisk},           {"6/9 Память", "memory", ModMemory},
        {"7/9 Сеть", "network", ModNetwork},     {"8/9 BSOD", "bsod", ModBsod},            {"9/9 Загрузка", "boot", ModBoot},
    };
    std::string base = c.outDir;
    for (const auto& s : steps) {
        if (c.cancelled()) { c.warn("Полная диагностика прервана."); return; }
        c.info(std::string("--- [") + s.name + "] ---");
        Ctx sc = c;
        sc.outDir = base + s.dir + "\\";
        MakeDirs(sc.outDir);
        try { s.fn(sc); }
        catch (const std::exception& ex) { c.error(std::string("Модуль ") + s.name + ": " + ex.what()); }
        catch (...) { c.error(std::string("Модуль ") + s.name + ": неизвестная ошибка"); }
    }
    Results& r = *c.res;
    HealthInput hi;
    hi.haveDisk = r.haveDisk;
    for (const auto& d : r.disks) hi.disks.push_back({d.model, d.health, d.wear, d.readErr, d.writeErr});
    hi.haveIntegrity = r.haveIntegrity; hi.integrityBad = r.integrityBad;
    hi.haveDrivers = r.haveDrivers; hi.errDevices = r.errDevices;
    hi.haveBsod = r.haveBsod; hi.bsodEvents = r.bsodEvents;
    hi.haveBoot = r.haveBoot; hi.bootIssues = r.bootIssues;
    HealthResult hr = computeHealth(hi);
    c.info(fmt("ОЦЕНКА ЗДОРОВЬЯ: %d / 100 (%s)", hr.score, hr.band.c_str()));
    for (const auto& s : hr.reasons) c.warn("  причина: " + s);

    Json full = Json::object();
    full.set("Timestamp", IsoNow()).set("OutDir", base).set("Score", hr.score).set("Band", hr.band);
    Json jr = Json::array();
    for (const auto& s : hr.reasons) jr.add(s);
    full.set("Reasons", jr);
    WriteText(base + "full.json", full.dump());
    std::string html = BuildHtmlReport(c, r, hr.score, hr.band, hr.reasons);
    std::string path = base + "report.html";
    if (WriteText(path, html)) { r.lastReportHtml = path; c.ok("HTML-отчёт: " + path); }
    else c.error("Не удалось записать HTML-отчёт.");
}

}  // namespace wr
