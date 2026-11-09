// Validates every shipped Playbooks/*.wrp. Build: g++ -std=c++17 tests/test_playbooks.cpp -o t && ./t
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include "../src/core/playbook_parse.h"
using namespace wr;
int main() {
    int bad = 0, n = 0;
    for (auto& e : std::filesystem::directory_iterator("Playbooks")) {
        if (e.path().extension() != ".wrp") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        Playbook pb;
        std::vector<std::string> errs;
        parsePlaybook(ss.str(), pb, errs);
        ++n;
        std::cout << e.path().filename().string() << ": " << (pb.valid ? "OK" : "ОШИБКИ") << " (шагов " << pb.steps.size() << ")\n";
        for (auto& x : errs) { std::cout << "   " << x << "\n"; ++bad; }
    }
    std::cout << n << " файлов, ошибок: " << bad << "\n";
    return bad ? 1 : 0;
}
