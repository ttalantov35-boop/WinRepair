// «Отменить изменения сеанса»: возврат состояния на начало сеанса по журналу резервных копий.
// Кусты реестра и файлы — обратная копия, живое хранилище BCD — bcdedit /import,
// карантин — перемещение на место, ветки реестра работающей Windows — reg import.
#pragma once
#include <string>
#include <vector>
#include "../win/backups.h"

namespace wr {

struct UndoReport {
    int ok = 0, failed = 0, skipped = 0;
    std::string text;  // построчный итог для диалога и журнала
};

std::string UndoSummary(const std::vector<SessionChange>& v);   // сгруппированный список для окна подтверждения
UndoReport UndoRestore(const std::vector<SessionChange>& v);    // выполняет откат; каждый шаг пишется в журнал

}  // namespace wr
