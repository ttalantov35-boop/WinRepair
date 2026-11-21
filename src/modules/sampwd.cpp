// Снятие пароля и разблокировка локальных учётных записей (SAM офлайн-Windows).
// Безопасность: перед правкой куст SAM копируется автоматически (Hives().load → RecordSessionChange),
// структуры V/F проверяются по размеру и RID; новый пароль не задаётся и в журнал не пишется.
#include "sampwd.h"
#include "../win/offreg.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace wr {
namespace {

// --- значение F (структура состояния учётной записи, стабильна Win7–Win11) ---
constexpr size_t F_RID = 0x30;  // DWORD — RID (для сверки)
constexpr size_t F_ACB = 0x38;  // WORD  — биты управления учётной записью
constexpr uint16_t ACB_DISABLED = 0x0001;
constexpr uint16_t ACB_PWNOTREQ = 0x0004;
constexpr uint16_t ACB_AUTOLOCK = 0x0400;

// --- значение V (заголовок — записи {offset,len,...}; данные начинаются с 0xCC) ---
constexpr size_t V_DATA   = 0xCC;
constexpr size_t V_NAME_O = 0x0C;  // имя: смещение
constexpr size_t V_NAME_L = 0x10;  // имя: длина
constexpr size_t V_FULL_O = 0x18;  // полное имя: смещение
constexpr size_t V_FULL_L = 0x1C;  // полное имя: длина
constexpr size_t V_LM_L   = 0xA0;  // длина LM-хеша
constexpr size_t V_NT_L   = 0xAC;  // длина NT-хеша

uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
    if (o + 4 > b.size()) return 0;
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}
uint16_t rd16(const std::vector<uint8_t>& b, size_t o) {
    if (o + 2 > b.size()) return 0;
    return (uint16_t)((uint16_t)b[o] | ((uint16_t)b[o + 1] << 8));
}
void wr32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    if (o + 4 > b.size()) return;
    b[o] = (uint8_t)(v & 0xFF); b[o + 1] = (uint8_t)((v >> 8) & 0xFF); b[o + 2] = (uint8_t)((v >> 16) & 0xFF); b[o + 3] = (uint8_t)((v >> 24) & 0xFF);
}
void wr16(std::vector<uint8_t>& b, size_t o, uint16_t v) {
    if (o + 2 > b.size()) return;
    b[o] = (uint8_t)(v & 0xFF); b[o + 1] = (uint8_t)((v >> 8) & 0xFF);
}

// строка UTF-16LE из значения V по паре полей (смещение, длина)
std::string VText(const std::vector<uint8_t>& v, size_t offField, size_t lenField) {
    uint32_t off = rd32(v, offField), len = rd32(v, lenField);
    size_t start = V_DATA + off;
    if (len == 0 || len > 1024 || start + len > v.size()) return "";
    return U((const wchar_t*)(v.data() + start), (int)(len / 2));
}

// найти и загрузить куст SAM цели (копия куста создаётся автоматически один раз за сеанс)
bool LoadSam(const Target& t, std::string& mount, std::string& err) {
    for (const auto& h : ListHives(t, false)) {
        if (h.label == "SAM") {
            if (!Hives().load(h, err)) return false;
            mount = h.mount;
            return true;
        }
    }
    err = "Куст SAM не найден в " + t.winDir;
    return false;
}

bool KeyIsRid(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}

// эвристика «учётная запись Microsoft»: наличие значений GivenName/SurName/Internet* под ключом учётки
bool LooksMicrosoft(HKEY users, const std::string& keyName) {
    HKEY uk = nullptr;
    if (RegOpenKeyExW(users, W(keyName).c_str(), 0, KEY_READ, &uk) != ERROR_SUCCESS) return false;
    std::vector<RegVal> vals;
    EnumValues(uk, vals);
    RegCloseKey(uk);
    for (const auto& rv : vals) {
        std::string ln = toLowerUtf8(rv.name);
        if (ln == "givenname" || ln == "surname" || ln.rfind("internet", 0) == 0) return true;
    }
    return false;
}

}  // namespace

bool SamListAccounts(const Target& t, std::vector<SamAccount>& out, std::string& err) {
    out.clear();
    if (t.online) { err = "Снятие пароля работает только для офлайн-Windows — загрузитесь с WinPE и выберите установленную Windows."; return false; }
    std::string mount;
    if (!LoadSam(t, mount, err)) return false;
    HKEY sam = Hives().open(mount, KEY_READ);
    if (!sam) { err = "Не удалось открыть загруженный куст SAM."; return false; }
    HKEY users = nullptr;
    const std::string usersPath = "SAM\\Domains\\Account\\Users";
    LONG r = RegOpenKeyExW(sam, W(usersPath).c_str(), 0, KEY_READ, &users);
    if (r != ERROR_SUCCESS) { RegCloseKey(sam); err = "В SAM не найден раздел учётных записей."; return false; }

    std::vector<std::string> subs;
    EnumSubKeys(users, subs);
    for (const auto& s : subs) {
        if (!KeyIsRid(s)) continue;  // пропускаем "Names" и прочее
        RegVal vv, vf;
        if (!QueryVal(users, s, "V", vv)) continue;
        bool haveF = QueryVal(users, s, "F", vf);
        SamAccount a;
        a.rid = (unsigned)strtoul(s.c_str(), nullptr, 16);
        a.name = VText(vv.data, V_NAME_O, V_NAME_L);
        a.fullName = VText(vv.data, V_FULL_O, V_FULL_L);
        a.hasPassword = rd32(vv.data, V_NT_L) > 0;
        a.builtinAdmin = (a.rid == 500);
        a.guest = (a.rid == 501);
        if (haveF && vf.data.size() >= F_ACB + 2) {
            uint16_t acb = rd16(vf.data, F_ACB);
            a.disabled = (acb & ACB_DISABLED) != 0;
            a.locked = (acb & ACB_AUTOLOCK) != 0;
            a.pwdNotReq = (acb & ACB_PWNOTREQ) != 0;
        }
        a.microsoft = LooksMicrosoft(users, s);
        if (!a.name.empty()) out.push_back(a);
    }
    RegCloseKey(users);
    RegCloseKey(sam);

    std::sort(out.begin(), out.end(), [](const SamAccount& x, const SamAccount& y) {
        if (x.builtinAdmin != y.builtinAdmin) return x.builtinAdmin;  // встроенный Администратор первым
        return toLowerUtf8(x.name) < toLowerUtf8(y.name);
    });
    return true;
}

bool SamApply(Ctx& c, unsigned rid, SamOp op, std::string& err) {
    const Target& t = c.target;
    if (t.online) { err = "Операция доступна только для офлайн-Windows."; return false; }
    std::string mount;
    if (!LoadSam(t, mount, err)) return false;
    HKEY sam = Hives().open(mount, KEY_READ | KEY_WRITE);
    if (!sam) { err = "Не удалось открыть SAM на запись."; return false; }

    std::string keyPath = fmt("SAM\\Domains\\Account\\Users\\%08X", rid);
    HKEY uk = nullptr;
    if (RegOpenKeyExW(sam, W(keyPath).c_str(), 0, KEY_READ | KEY_WRITE, &uk) != ERROR_SUCCESS) {
        RegCloseKey(sam);
        err = fmt("Учётная запись (RID %u) не найдена в SAM.", rid);
        return false;
    }

    RegVal vv, vf;
    bool haveV = QueryVal(uk, "", "V", vv);
    bool haveF = QueryVal(uk, "", "F", vf);
    std::string name = (haveV ? VText(vv.data, V_NAME_O, V_NAME_L) : std::string());
    if (name.empty()) name = fmt("RID %u", rid);

    // сверка структуры: RID в F должен совпадать — иначе формат не распознан
    if (haveF && vf.data.size() >= F_RID + 4) {
        uint32_t fr = rd32(vf.data, F_RID);
        if (fr != 0 && fr != rid) {
            RegCloseKey(uk); RegCloseKey(sam);
            err = "Структура SAM не распознана (RID не совпадает) — операция отменена ради безопасности.";
            return false;
        }
    }

    bool wantBlank = (op == SamOp::Blank || op == SamOp::BlankEnableUnlock);
    bool wantEnable = (op == SamOp::Enable || op == SamOp::BlankEnableUnlock);
    bool wantUnlock = (op == SamOp::Unlock || op == SamOp::BlankEnableUnlock);

    bool vChanged = false, fChanged = false;

    if (wantBlank) {
        if (!haveV || vv.data.size() < V_NT_L + 4) {
            RegCloseKey(uk); RegCloseKey(sam);
            err = "Значение V учётной записи имеет неожиданный размер — снятие пароля отменено.";
            return false;
        }
        if (rd32(vv.data, V_LM_L) != 0) { wr32(vv.data, V_LM_L, 0); vChanged = true; }
        if (rd32(vv.data, V_NT_L) != 0) { wr32(vv.data, V_NT_L, 0); vChanged = true; }
    }

    if (haveF && vf.data.size() >= F_ACB + 2) {
        uint16_t acb = rd16(vf.data, F_ACB), na = acb;
        if (wantBlank) na |= ACB_PWNOTREQ;       // пароль не требуется
        if (wantEnable) na &= (uint16_t)~ACB_DISABLED;
        if (wantUnlock) na &= (uint16_t)~ACB_AUTOLOCK;
        if (na != acb) { wr16(vf.data, F_ACB, na); fChanged = true; }
    } else if (wantEnable || wantUnlock) {
        RegCloseKey(uk); RegCloseKey(sam);
        err = "Значение F учётной записи не найдено или слишком мало — изменение состояния отменено.";
        return false;
    }

    LONG rv = ERROR_SUCCESS, rf = ERROR_SUCCESS;
    if (vChanged) rv = RegSetRaw(uk, "V", vv.type ? vv.type : REG_BINARY, vv.data);
    if (fChanged) rf = RegSetRaw(uk, "F", vf.type ? vf.type : REG_BINARY, vf.data);
    RegFlushKey(uk);
    RegCloseKey(uk);
    RegFlushKey(sam);
    RegCloseKey(sam);

    if (rv != ERROR_SUCCESS || rf != ERROR_SUCCESS) {
        err = "Не удалось записать изменения в SAM: " + ErrMsg((DWORD)(rv != ERROR_SUCCESS ? rv : rf));
        return false;
    }
    if (!vChanged && !fChanged) {
        c.info("Для «" + name + "» изменять нечего — состояние уже соответствует.");
        return true;
    }

    std::string act;
    auto add = [&](const std::string& s) { act += (act.empty() ? "" : "; ") + s; };
    if (wantBlank) add("пароль снят");
    if (wantEnable) add("включена");
    if (wantUnlock) add("разблокирована");

    AuditChange(fmt("Учётная запись «%s» (RID %u): %s (SAM тома %c:, копия куста сохранена; новый пароль не задаётся).",
                    name.c_str(), rid, act.c_str(), (char)toupper((unsigned char)t.letter())));
    c.ok("Готово: «" + name + "» — " + act + ". Войдите " + (wantBlank ? "с пустым паролем (поле пароля оставьте пустым)." : "в учётную запись."));
    return true;
}

}  // namespace wr
