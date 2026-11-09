#!/bin/sh
# Platform-independent core tests (Linux/WSL/MSYS2): ./run_tests.sh
g++ -std=c++17 -Wall -Wextra tests/test_core.cpp -o /tmp/wr_core_test && /tmp/wr_core_test
g++ -std=c++17 -Wall -Wextra tests/test_playbooks.cpp -o /tmp/wr_pb_test && /tmp/wr_pb_test
g++ -std=c++17 -Wall -Wextra tests/test_lang.cpp -o /tmp/wr_lang_test && /tmp/wr_lang_test
python3 tools/i18n_check.py
python3 - <<'PY'
import glob,sys
rc=open('res/app.rc',encoding='utf-8').read()
miss=[f for f in sorted(glob.glob('Playbooks/*.wrp')) if f.split('/')[-1] not in rc]
print('playbooks not embedded in app.rc:', miss)
sys.exit(1 if miss else 0)
PY
