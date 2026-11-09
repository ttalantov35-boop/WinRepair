// Loads lang/en.txt with the real dictionary engine and checks several known entries. Run from the project root.
#include <fstream>
#include <iostream>
#include <sstream>
#include "../src/core/i18n.h"
using namespace wr;
static int fails = 0;
#define CHECK_EQ(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { std::cout << "FAIL: [" << _a << "] != [" << _b << "]\n"; ++fails; } } while (0)
int main() {
    std::ifstream f("lang/en.txt", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Dict d;
    size_t n = d.load(ss.str());
    std::cout << "entries loaded: " << n << "\n";
    CHECK_EQ(d.tr("Обзор"), "Overview");
    CHECK_EQ(d.tr("Диски"), "Disks");
    CHECK_EQ(d.tr("Отмена"), "Cancel");
    CHECK_EQ(d.tr("Томов: 3, заблокировано: 1. Разблокировка нужна, если Windows на диске не находится в списке «Целевая ОС»."), "Volumes: 3, locked: 1. Unlocking is needed if the Windows on the disk is not in the \"Target OS\" list.");
    CHECK_EQ(d.tr("ПРОБЛЕМА: Том C: заблокирован BitLocker"), d.tr("ПРОБЛЕМА: Том C: заблокирован BitLocker"));
    CHECK_EQ(d.tr("Неведомая русская фраза без перевода"), "Неведомая русская фраза без перевода");
    CHECK_EQ(d.tr("plain ascii"), "plain ascii");
    if (n < 1000) { std::cout << "FAIL: dictionary too small\n"; ++fails; }
    std::cout << (fails ? "LANG TESTS FAILED\n" : "LANG TESTS PASSED\n");
    return fails ? 1 : 0;
}
