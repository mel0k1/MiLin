#!/usr/bin/env python3
"""fix-dead-refs.py — чистит Kbuild/Kconfig от ссылок на удалённые каталоги.

Использование: fix-dead-refs.py <top-of-linux-tree>

После rm -rf подсистемы в дереве остаются висячие ссылки:
  - Kconfig:  source "sound/Kconfig"     -> kconfig падает на парсинге;
  - Makefile: obj-$(CONFIG_X) += sound/  -> kbuild пытается спуститься
    в удалённый каталог (vmlinux-dirs).

Правила (консервативные, чтобы не трогать лишнее):
  - Kconfig: вычищаем ТОЛЬКО строки source "путь" (без переменных $(...));
  - Makefile: вычищаем ТОЛЬКО строки-присваивания (obj-y += foo/ и т.п.),
    комментарии, рецепты, make-паттерны (%/) и неприсваивания не трогаем;
  - пути проверяем относительно каталога самого Makefile, а если там нет —
    относительно корня дерева (arch/*/Makefile инклудится топ-Makefile и
    его пути корневые);
  - токены с $ не разрешаем (это переменные);
  - склеенный оператор (+=nouveau/) сохраняет оператор.

Скрипт идемпотентен.
"""
import os
import re
import sys

TOP = os.path.realpath(sys.argv[1])
SKIP_DIRS = {'.git'}
_changed_files = 0

SOURCE_RE = re.compile(r'^(\s*source\s+)"?([^"\s]+)"?(\s*)$')
# строка-присваивание kbuild: имя(с $(CONFIG_*) внутри) := / += / =
ASSIGN_RE = re.compile(r'^[A-Za-z0-9_][A-Za-z0-9_$.()\-]*[ \t]*(\+=|:=|=)[ \t]*')
# склеенный оператор+путь: +=nouveau/ -> op '+=', path 'nouveau/'
GLUED_OP_RE = re.compile(r'^([+?:!]?=|:=|=)(\S+)$')
TOKEN_SPLIT = re.compile(r'[ \t]+')


def _walk(top):
    for root, dirs, files in os.walk(top):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        yield root, dirs, files


def kconfig_files():
    for root, dirs, files in _walk(TOP):
        if os.path.realpath(root) == os.path.join(TOP, 'scripts'):
            dirs[:] = []
            continue
        for f in files:
            if f == 'Kconfig' or f.startswith('Kconfig.'):
                yield os.path.join(root, f)


def makefile_files():
    for root, dirs, files in _walk(TOP):
        if os.path.realpath(root) in (
                os.path.join(TOP, 'scripts'),
                os.path.join(TOP, 'tools')):
            dirs[:] = []
            continue
        for f in files:
            if f in ('Makefile', 'Kbuild'):
                yield os.path.join(root, f)


def fix_kconfig(path):
    global _changed_files
    with open(path, encoding='utf-8', errors='replace') as fh:
        lines = fh.readlines()
    out, changed = [], False
    for line in lines:
        m = SOURCE_RE.match(line.rstrip('\n'))
        if m:
            target = m.group(2)
            if '$(' in target:
                out.append(line)
                continue
            if target.startswith('$(srctree)/'):
                exists = os.path.exists(
                    os.path.join(TOP, target[len('$(srctree)/'):]))
            else:
                exists = (
                    os.path.exists(os.path.join(os.path.dirname(path), target))
                    or os.path.exists(os.path.join(TOP, target)))
            if not exists:
                changed = True
                print(f"  kconfig: {os.path.relpath(path, TOP)}: "
                      f"drop source -> {target}")
                continue
        out.append(line)
    if changed:
        with open(path, 'w', encoding='utf-8') as fh:
            fh.writelines(out)
        _changed_files += 1


def fix_makefile(path):
    global _changed_files
    base = os.path.dirname(path)

    def dir_exists(tok):
        # пути в kbuild: локальные (от каталога Makefile) или корневые
        return (os.path.isdir(os.path.normpath(os.path.join(base, tok)))
                or os.path.isdir(os.path.normpath(os.path.join(TOP, tok))))

    with open(path, encoding='utf-8', errors='replace') as fh:
        lines = fh.readlines()
    out, changed = [], False
    for line in lines:
        stripped = line.rstrip('\n')
        # рецепты, комментарии и неприсваивания не трогаем
        if stripped.startswith('\t') or stripped.lstrip().startswith('#') \
                or not ASSIGN_RE.match(stripped):
            out.append(line)
            continue
        parts = TOKEN_SPLIT.split(stripped)
        new_parts, removed = [], False
        for tok in parts:
            glued = GLUED_OP_RE.match(tok)
            if glued:
                d = glued.group(2)
                if d.endswith('/') and len(d) > 1 and '$' not in d \
                        and not dir_exists(d):
                    removed = True
                    print(f"  kbuild: {os.path.relpath(path, TOP)}: "
                          f"drop dir -> {d}")
                    new_parts.append(glued.group(1))
                    continue
                new_parts.append(tok)
                continue
            if tok.startswith('%'):
                new_parts.append(tok)  # make-паттерн типа %/
                continue
            if tok.endswith('/') and len(tok) > 1 and '$' not in tok:
                if not dir_exists(tok):
                    removed = True
                    print(f"  kbuild: {os.path.relpath(path, TOP)}: "
                          f"drop dir -> {tok}")
                    continue
            new_parts.append(tok)
        if removed:
            rebuilt = ' '.join(p for p in new_parts if p != '')
            out.append(rebuilt + '\n')
            changed = True
        else:
            out.append(line)
    if changed:
        with open(path, 'w', encoding='utf-8') as fh:
            fh.writelines(out)
        _changed_files += 1


if __name__ == '__main__':
    for p in kconfig_files():
        fix_kconfig(p)
    for p in makefile_files():
        fix_makefile(p)
    print(f"fix-dead-refs: {_changed_files} file(s) cleaned")
