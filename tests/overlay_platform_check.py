"""检查独立窗口的平台菜单开关和兼容配置，无需游戏或图形环境。"""
import argparse
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    menu = (root / 'runtime/ui/menu.cpp').read_text(encoding='utf-8')
    begin = menu.index('    opt_choice("TOTK_PERF_OVERLAY"')
    end = menu.index('    opt_group(UI_MENU_GRAPHICS);', begin)
    fragment = menu[begin:end]
    keys = re.findall(r'opt_toggle\("TOTK_((?:PERF|INPUT)_WINDOW)"', fragment)
    assert 'PERF_WINDOW' in keys
    for windows in (False, True):
        result = subprocess.run(
            [args.cc, '-E', '-P', '-x', 'c', '-U_WIN32'] +
            (['-D_WIN32=1'] if windows else []) + ['-'],
            input=fragment, text=True, capture_output=True, check=True,
        ).stdout
        assert 'TOTK_PERF_OVERLAY' in result
        for key in keys:
            assert ('TOTK_' + key in result) == windows, (windows, key)
    cfg = (root / 'runtime/cfg.c').read_text(encoding='utf-8')
    known = cfg.split('known[] = {', 1)[1].split('};', 1)[0]
    for key in keys:
        assert '"' + key + '"' in known
        assert re.search(r'\{"' + key + r'", "0", "0\|1", LIVE\}', cfg)
    print('PASS: Windows 显示窗口开关；非 Windows 隐藏窗口开关但保留游戏内浮层及兼容配置')


if __name__ == '__main__':
    main()
