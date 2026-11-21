// Full diagnostic (9 modules) + health score + HTML report (port of Invoke-WRFullDiagnostic / Export-WRHtmlReport).
#include "../win/app.h"
#include "../core/health.h"
#include "../core/bsod_catalog.h"

namespace wr {

namespace {

// Что проверяет раздел и как читать его строки (показывается под заголовком раздела).
const char* SectionIntro(const std::string& title) {
    static const struct { const char* t; const char* intro; } m[] = {
        {"Система", "Общие сведения: версия Windows, время работы, объём памяти, BIOS/UEFI, Secure Boot и TPM. Нужны, чтобы подобрать драйверы и понять, на каком оборудовании работает система."},
        {"События", "Ошибки и критические события журналов System и Application за выбранный период. Одинаковые события сгруппированы: у каждой группы — сколько раз, когда последний раз, что это значит и что проверить."},
        {"Целостность", "Проверка системных файлов Windows (SFC) и хранилища компонентов (DISM). «0 — успешно» — нарушений нет. Если найдены нарушения — используйте «ИСПРАВИТЬ целостность (DISM + SFC)»."},
        {"Драйверы", "Устройства, у которых Диспетчер устройств показывает проблему. Для каждого — код проблемы, что он значит и что делать. Код 22 — устройство просто отключено."},
        {"Диски", "Физические диски и их состояние (SMART): здоровье, износ SSD, ошибки чтения и записи, температура, а также события дисков и файловой системы. Ошибки диска — повод сначала сохранить данные."},
        {"Память", "Модули оперативной памяти по данным SMBIOS: слоты, объём, тип и частота. Программа память не тестирует — для теста нужен отдельный тест памяти (JEDEC, несколько проходов)."},
        {"Сеть", "Сетевые адаптеры, адреса, DNS и доступ к указанному хосту и порту. Замечания показывают, на каком шаге пропадает связь."},
        {"BSOD", "Синие экраны (события BugCheck) и найденные файлы дампов. Для каждого кода — что он значит, первый шаг и что важно. Код BSOD — категория, а не диагноз."},
        {"Загрузка", "Разметка диска (GPT/MBR), раздел EFI, загрузчик и среда восстановления WinRE. Замечания показывают, что может помешать загрузке."},
        {"Дампы", "Разбор файлов дампов памяти: код остановки, параметры и (если есть cdb.exe) модуль, который вызвал сбой."},
    };
    for (auto& e : m) if (title == e.t) return e.intro;
    return "";
}

std::string Multiline(const std::string& s) {   // \n → <br>, текст экранирован
    std::string e = htmlEscape(s), o;
    for (char ch : e) {
        if (ch == '\n') o += "<br>";
        else if (ch != '\r') o += ch;
    }
    return o;
}

// Подсветка строки: проблема / норма / нейтрально.
const char* RowClass(const std::string& v) {
    std::string l = toLowerUtf8(v);
    if (l.rfind("код 22 ", 0) == 0) return " class='info'";   // устройство просто отключено
    if (l.find("нарушени") != std::string::npos || l.find("проблем") != std::string::npos || l.find("bugcheck") != std::string::npos ||
        l.find("ошибк") != std::string::npos || l.find("повреж") != std::string::npos)
        return " class='bad'";
    if (l.rfind("код ", 0) == 0) return " class='bad'";
    if (l.rfind("нет —", 0) == 0 || l.rfind("0 — успешно", 0) == 0 || l.find("замечаний нет") != std::string::npos) return " class='ok'";
    return "";
}

}  // namespace

std::string BuildHtmlReport(const Ctx& c, const Results& r, int score, const std::string& band, const std::vector<std::string>& reasons) {
    const Env& e = GetEnv();
    std::string h;
    h += "<!DOCTYPE html><html lang=\"" + std::string(CurrentLang() == Lang::EN ? "en" : "ru") + "\"><head><meta charset=\"utf-8\"><title>WinRepair " + htmlEscape(NiceNow()) + "</title><style>"
         "body{font-family:Segoe UI,Arial,sans-serif;margin:24px auto;max-width:1100px;color:#1a1a1a;background:#f6f7f9;line-height:1.45}h1{color:#0a4d8c;margin-bottom:4px}"
         "h2{color:#0a4d8c;border-bottom:2px solid #0a4d8c;padding-bottom:4px;margin-top:36px}"
         ".meta{color:#555}.box{background:#fff;border:1px solid #dde3ea;border-radius:8px;padding:12px 16px;margin:12px 0}"
         ".howto{background:#eef6ff;border-color:#c9def5}.intro{color:#333;background:#fff;border-left:4px solid #0a4d8c;padding:8px 12px;margin:8px 0}"
         ".caveat{background:#fff7d6;border-left:4px solid #c99400;padding:8px 12px;margin:8px 0;color:#5a4300}"
         "table{border-collapse:collapse;width:100%;margin:8px 0;background:#fff}th,td{border:1px solid #dde3ea;padding:7px 10px;text-align:left;font-size:14px;vertical-align:top}"
         "th{background:#eef4fa;width:32%;font-weight:600}tr.bad td{background:#fff1f0}tr.bad th{background:#fde2df;border-left:4px solid #c62828}"
         "tr.ok td{background:#f1fbf3}tr.ok th{border-left:4px solid #2e7d32}tr.info th{border-left:4px solid #8a94a3}"
         ".score{font-size:42px;font-weight:bold;padding:16px;border-radius:8px;display:inline-block}"
         ".Excellent{background:#d4edda;color:#155724}.Good{background:#d1ecf1;color:#0c5460}.Fair{background:#fff3cd;color:#856404}"
         ".Poor{background:#f8d7da;color:#721c24}.Critical{background:#721c24;color:#fff}.note{color:#666;font-size:13px}"
         "ul.toc{columns:2;list-style:none;padding:0}ul.toc li{margin:2px 0}a{color:#0a4d8c}"
         "</style></head><body>";
    h += "<h1>" + Tr("WinRepair — отчёт") + "</h1><p class='meta'>" + htmlEscape(NiceNow()) + " · " + htmlEscape(e.computer) + " · " + htmlEscape(c.target.label) +
         " · " + htmlEscape(std::string(kAppName) + " " + kAppVersion) + "</p>";
    h += "<div class='box howto'><b>" + Tr("Как читать отчёт") + "</b><br>" +
         Multiline(Tr("Каждый раздел начинается с пояснения: что проверялось. Строки на красном фоне — найденные проблемы, на зелёном — проверка пройдена. "
                      "У кодов (устройств, событий, синих экранов, DISM/SFC) рядом написано, что они значат и что делать дальше.\n"
                      "Оценка здоровья — ориентир для быстрого решения, а не диагноз: окончательный вывод делает инженер.")) + "</div>";
    h += fmt(("<h2>" + Tr("Оценка") + "</h2><div class='score %s'>%d / 100 &nbsp; %s</div>").c_str(), band.c_str(), score, band.c_str());
    if (!reasons.empty()) {
        h += "<p><b>" + Tr("Что снизило оценку:") + "</b></p><ul>";
        for (const auto& s : reasons) h += "<li>" + htmlEscape(Tr(s)) + "</li>";
        h += "</ul>";
    } else h += "<p>" + Tr("Явных проблем, снижающих оценку, не найдено.") + "</p>";
    h += "<p class='note'>" + Tr("Шкала: 90–100 — отлично, 75–89 — хорошо, 50–74 — удовлетворительно, 25–49 — плохо, ниже 25 — критично. Синтетическая оценка для клиента; не заменяет решение инженера.") + "</p>";
    if (!r.sections.empty()) {
        h += "<div class='box'><b>" + Tr("Разделы") + "</b><ul class='toc'>";
        for (size_t i = 0; i < r.sections.size(); ++i) h += "<li><a href='#s" + std::to_string(i) + "'>" + htmlEscape(Tr(r.sections[i].title)) + "</a></li>";
        h += "</ul></div>";
    }
    for (size_t i = 0; i < r.sections.size(); ++i) {
        const auto& s = r.sections[i];
        h += "<h2 id='s" + std::to_string(i) + "'>" + htmlEscape(Tr(s.title)) + "</h2>";
        const char* intro = SectionIntro(s.title);
        if (*intro) h += "<div class='intro'>" + htmlEscape(Tr(intro)) + "</div>";
        if (s.title == "BSOD" || s.title == "Дампы") h += "<div class='caveat'>" + htmlEscape(bsodCaveat(CurrentLang() == Lang::EN)) + "</div>";
        h += "<table>";
        for (const auto& row : s.rows)
            h += std::string("<tr") + RowClass(row.second) + "><th>" + Multiline(Tr(row.first)) + "</th><td>" + Multiline(Tr(row.second)) + "</td></tr>";
        h += "</table>";
        for (const auto& n : s.notes) h += "<p class='note'>" + Multiline(Tr(n)) + "</p>";
    }
    h += "<p class='note'>" + Tr("Сформировано") + " " + std::string(kAppName) + " " + kAppVersion + " · " + WR_COMPANY + "</p></body></html>";
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
