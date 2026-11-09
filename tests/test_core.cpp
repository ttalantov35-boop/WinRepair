// Unit tests for the platform-independent core. Build: g++ -std=c++17 -Wall -Wextra tests/test_core.cpp -o t && ./t
#include <cassert>
#include <cstdio>
#include "../src/core/textutil.h"
#include "../src/core/json.h"
#include "../src/core/smbios.h"
#include "../src/core/dumpheader.h"
#include "../src/core/health.h"
#include "../src/core/bsod_catalog.h"
#include "../src/core/bcdparse.h"
#include "../src/core/playbook_parse.h"
#include "../src/core/i18n.h"
#include "../src/core/sha256.h"
using namespace wr;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void addStr(std::vector<uint8_t>& v, const char* s) { while (*s) v.push_back((uint8_t)*s++); v.push_back(0); }

int main() {
    // --- JSON
    Json j = Json::object();
    j.set("a", 1).set("b", "x\"y\n").set("c", Json::array().add(true).add(2.5));
    j.set("a", 7);
    std::string s = j.dump(0);
    CHECK(s == "{\"a\":7,\"b\":\"x\\\"y\\n\",\"c\":[true,2.5]}\n");

    // --- UTF-16 detection / conversion (sfc output)
    const uint8_t u16[] = {'W',0,'i',0,'n',0,' ',0,0x3F,0x04,0x40,0x04,0x0D,0,0x0A,0}; // "Win пр\r\n"
    CHECK(looksLikeUtf16le(u16, sizeof u16));
    CHECK(utf16leToUtf8(u16, sizeof u16) == "Win пр\r\n");
    const uint8_t oem[] = {'a','b','c','d','e','f'};
    CHECK(!looksLikeUtf16le(oem, sizeof oem));

    // --- lowercase / integrity keywords
    CHECK(toLowerUtf8("ЗАЩИТА Ёж") == "защита еж");
    CHECK(integrityViolationFound("Windows Resource Protection found integrity violations."));
    CHECK(!integrityViolationFound("Windows Resource Protection did not find any integrity violations."));
    CHECK(integrityViolationFound("Защита ресурсов Windows обнаружила нарушения целостности."));
    CHECK(!integrityViolationFound("Защита ресурсов Windows не обнаружила нарушений целостности."));
    CHECK(integrityViolationFound("The component store is repairable."));
    CHECK(!integrityViolationFound("No component store corruption detected."));

    // --- helpers
    uint32_t code = 0;
    CHECK(parseHexCode("0x00000133 (0x0000000000000001, 0x2)", code) && code == 0x133);
    CHECK(fieldValue("BUGCHECK_CODE:  133\nMODULE_NAME: nvlddmkm\n", "MODULE_NAME:") == "nvlddmkm");
    CHECK(bsodByCode(0x133) && std::string(bsodByCode(0x133)->name) == "DPC_WATCHDOG_VIOLATION");
    CHECK(truncUtf8("привет", 3) == "п");

    // --- SMBIOS: type0 + type17 (one DDR4 module 16GB, 3200) + type127
    std::vector<uint8_t> t;
    { // type 0
        uint8_t h[0x18] = {0}; h[0] = 0; h[1] = 0x18; h[4] = 1; h[5] = 2; h[8] = 3;
        t.insert(t.end(), h, h + 0x18);
        addStr(t, "AMI"); addStr(t, "F12"); addStr(t, "01/02/2023"); t.push_back(0);
    }
    { // type 17
        uint8_t h[0x28] = {0}; h[0] = 17; h[1] = 0x28;
        h[0x0C] = 0x00; h[0x0D] = 0x40; // 16384 MB
        h[0x10] = 1; h[0x11] = 2; h[0x12] = 0x1A;
        h[0x15] = 0x80; h[0x16] = 0x0C; // 3200
        h[0x17] = 3; h[0x18] = 4; h[0x1A] = 5;
        h[0x20] = 0x80; h[0x21] = 0x0C;
        t.insert(t.end(), h, h + 0x28);
        addStr(t, "DIMM A1"); addStr(t, "BANK 0"); addStr(t, "Kingston"); addStr(t, "SN123"); addStr(t, "KF3200"); t.push_back(0);
    }
    { uint8_t h[4] = {127, 4, 0, 0}; t.insert(t.end(), h, h + 4); t.push_back(0); t.push_back(0); }
    std::vector<uint8_t> raw = {0, 3, 3, 0, (uint8_t)(t.size() & 0xFF), (uint8_t)(t.size() >> 8), 0, 0};
    raw.insert(raw.end(), t.begin(), t.end());
    SmbInfo si = parseSmbios(raw.data(), raw.size());
    CHECK(si.ok && si.biosVendor == "AMI" && si.biosVersion == "F12" && si.biosDate == "01/02/2023");
    CHECK(si.mem.size() == 1);
    if (si.mem.size() == 1) {
        CHECK(si.mem[0].sizeMB == 16384 && si.mem[0].speed == 3200 && si.mem[0].configSpeed == 3200);
        CHECK(si.mem[0].locator == "DIMM A1" && si.mem[0].manufacturer == "Kingston" && si.mem[0].part == "KF3200");
        CHECK(std::string(smbMemTypeName(si.mem[0].type)) == "DDR4");
    }

    // --- dump header x64
    std::vector<uint8_t> d(0x1000, 0);
    memcpy(d.data(), "PAGEDU64", 8);
    d[0x38] = 0x33; d[0x39] = 0x01; d[0x40] = 0x01; d[0xF98] = 2;
    DumpInfo di = parseDumpHeader(d.data(), d.size());
    CHECK(di.ok && di.is64 && di.bugcheck == 0x133 && di.param[0] == 1 && di.dumpType == 2);
    CHECK(!parseDumpHeader((const uint8_t*)"MDMP0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000", 100).ok);

    // --- health score
    HealthInput hi;
    hi.haveDisk = true;
    hi.disks.push_back({"D1", "Warning", 85, 3, 0});
    hi.haveIntegrity = hi.integrityBad = true;
    hi.haveBsod = true; hi.bsodEvents = 5;
    HealthResult hr = computeHealth(hi);
    CHECK(hr.score == 0);  // 100-25-15-20-25-30 clamps to 0
    CHECK(hr.band == "Critical");
    HealthInput ok; ok.haveDisk = true; ok.disks.push_back({"D", "Healthy", 10, 0, 0});
    CHECK(computeHealth(ok).score == 100 && computeHealth(ok).band == "Excellent");

    // --- bcdedit parser
    const char* bcd = "Windows Boot Manager\r\n--------------------\r\nidentifier              {bootmgr}\r\ndescription             Windows Boot Manager\r\n"
        "displayorder            {current}\r\n                        {abc-123}\r\ntimeout                 30\r\n\r\n"
        "Windows Boot Loader\r\n-------------------\r\nidentifier              {current}\r\ndevice                  partition=C:\r\ndescription             Windows 10\r\nsafeboot                Minimal\r\n";
    auto be = parseBcdEnum(bcd);
    CHECK(be.size() == 2);
    if (be.size() == 2) {
        CHECK(be[0].id() == "{bootmgr}" && be[0].heading == "Windows Boot Manager");
        CHECK(be[0].get("displayorder") == "{current}, {abc-123}");
        CHECK(be[1].id() == "{current}" && be[1].desc() == "Windows 10" && be[1].get("SafeBoot") == "Minimal");
    }
    const char* bcdRu = "Диспетчер загрузки Windows\n---------------------------\nИдентификатор           {bootmgr}\nОписание                Диспетчер\n";
    auto br = parseBcdEnum(bcdRu);
    CHECK(br.size() == 1 && br[0].id() == "{bootmgr}" && br[0].desc() == "Диспетчер");

    // --- playbook parser
    const char* pbText = "# demo\n[playbook]\nid = Test-One\ntitle = Проверка\nkind = repair\nrisk = changing\ndescription = строка 1\n  строка 2\n\n"
        "[step]\ntype = message\ntext = Привет {VOL}\n\n[step]\ntype = cmd\nrun = dism.exe /Image:{TARGET} /X\nrisk = changing\n";
    Playbook pbk;
    std::vector<std::string> perr;
    CHECK(parsePlaybook(pbText, pbk, perr));
    CHECK(pbk.id == "test-one" && pbk.steps.size() == 2 && pbk.description == "строка 1\nстрока 2");
    CHECK(pbk.steps[1].get("run") == "dism.exe /Image:{TARGET} /X");
    std::map<std::string, std::string> vv = {{"VOL", "C:"}, {"TARGET", "C:\\"}};
    CHECK(expandVars("a {vol} b {TARGET}x {nope}", vv) == "a C: b C:\\x {nope}");
    Playbook bad;
    std::vector<std::string> berr;
    CHECK(!parsePlaybook("[playbook]\nid = x y\ntitle = T\nkind = triage\n[step]\ntype = cmd\nrun = a\n[step]\ntype = zzz\n", bad, berr));
    CHECK(berr.size() >= 3);

    // --- i18n dictionary
    Dict dict;
    dict.load("# test\nЗагрузка ==> Boot\nТомов: {}, заблокировано: {} ==> Volumes: {}, locked: {}\nОшибка: {} ==> Error: {}\n{} не найден ==> {} not found\n"
              "Строка\\nдве ==> Line\\ntwo\nДиск {}: {} ==> Disk {}: {}\n");
    CHECK(dict.tr("Загрузка") == "Boot");
    CHECK(dict.tr("Томов: 3, заблокировано: 1") == "Volumes: 3, locked: 1");
    CHECK(dict.tr("Ошибка: Загрузка") == "Error: Boot");           // captured value is translated too
    CHECK(dict.tr("Winre.wim не найден") == "Winre.wim not found");
    CHECK(dict.tr("Строка\nдве") == "Line\ntwo");
    CHECK(dict.tr("Disk 0: X") == "Disk 0: X");                      // no Cyrillic -> untouched
    CHECK(dict.tr("Неизвестная фраза") == "Неизвестная фраза");      // untranslated falls back to the original
    CHECK(dict.tr("Диск 0: Загрузка") == "Disk 0: Boot");
    Dict short_; short_.load("{} в {} ==> {} in {}\n");
    CHECK(short_.tr("Это окно в реестре") == "Это окно в реестре");  // short templates do not capture Cyrillic text
    CHECK(short_.tr("5 в 7") == "5 in 7");                          // numbers / ASCII values are captured

    // --- SHA-256 (FIPS 180-4 vectors) + manifest
    CHECK(Sha256::of("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::of("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        CHECK(Sha256::of(m, strlen(m)) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        std::string million(1000000, 'a');
        CHECK(Sha256::of(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        Sha256 inc; for (size_t i = 0; i < strlen(m); i++) inc.update(m + i, 1);
        CHECK(inc.hex() == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    }
    {
        std::string man = "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD *WinRepair_x64.exe\r\n"
                          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  Lang/en.txt\n";
        CHECK(ManifestLookup(man, "winrepair_x64.exe") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CHECK(ManifestLookup(man, "Lang/en.txt") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        CHECK(ManifestLookup(man, "missing.exe").empty());
    }

    printf(fails ? "FAILED: %d\n" : "ALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
