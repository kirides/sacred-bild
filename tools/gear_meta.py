"""Reads Sacred Gear's LuaLS definition files (gear/meta/*.lua) into a small model.

Used by gen_gear_api_docs.py (writes docs/GearApi.md) and check_gear_api.py (compares the model with the kernel).
Only the annotation idioms those files use are understood; see docs/GearApi.md, "Maintaining the annotations".
"""
import os
import re

# The meta files in the order they appear in the reference.
FILES = ['gear.lua', 'data.lua', 'world.lua', 'game.lua', 'inv.lua', 'ui.lua', 'ui_widgets.lua', 'auto.lua']


class Param:
    def __init__(self, name, type_, desc):
        self.name, self.type, self.desc = name, type_, desc
        self.optional = name.endswith('?') or type_.endswith('?')
        self.name = name.rstrip('?')


class Func:
    def __init__(self, owner, name, colon, desc, params, returns, overloads, deprecated, args):
        self.owner, self.name, self.colon = owner, name, colon
        self.desc, self.params, self.returns = desc, params, returns
        self.overloads, self.deprecated, self.args = overloads, deprecated, args


class Cls:
    def __init__(self, name, parent, desc):
        self.name, self.parent, self.desc = name, parent, desc
        self.fields = []          # Param
        self.funcs = []           # Func
        self.path = None


class Alias:
    def __init__(self, name, type_, desc):
        self.name, self.type, self.desc, self.values = name, type_, desc, []


class Section:
    def __init__(self, title):
        self.title, self.intro, self.items = title, [], []   # items: Cls | Alias | Func (in file order)


class Model:
    def __init__(self):
        self.sections = []
        self.classes = {}
        self.aliases = {}
        self.globals = []         # Func without an owner class
        self.var_class = {}       # local/global variable name -> class name

    def class_members(self, name, seen=None):
        """Member names of a class (fields and functions), parents included."""
        seen = seen or set()
        if name in seen or name not in self.classes:
            return set()
        seen.add(name)
        c = self.classes[name]
        out = {f.name for f in c.fields} | {f.name for f in c.funcs}
        if c.parent:
            out |= self.class_members(c.parent, seen)
        return out


def split_type(s):
    """Splits '<type> <rest>' where the type may hold spaces inside brackets and after a ':' (fun(): x)."""
    depth = 0
    i = 0
    s = s.strip()
    while i < len(s):
        ch = s[i]
        if ch in '({[':
            depth += 1
        elif ch in ')}]':
            depth -= 1
        elif ch == "'" or ch == '"':
            j = s.find(ch, i + 1)
            i = j if j >= 0 else len(s)
        elif ch.isspace() and depth == 0:
            prev = s[:i].rstrip()[-1:]
            nxt = s[i:].lstrip()[:1]
            if prev not in (':', '|', ',') and nxt not in ('|',):
                return s[:i], s[i:].strip()
        i += 1
    return s, ''


def join_doc(lines):
    out, para = [], []
    for ln in lines:
        if ln == '':
            if para:
                out.append(' '.join(para))
                para = []
        else:
            para.append(ln)
    if para:
        out.append(' '.join(para))
    return '\n\n'.join(out)


def parse(meta_dir):
    model = Model()
    for fname in FILES:
        path = os.path.join(meta_dir, fname)
        with open(path, encoding='utf-8') as f:
            lines = f.read().split('\n')
        parse_file(model, lines, fname)
    return model


def parse_file(model, lines, fname):
    section = None
    doc, tags = [], []
    pending_path = None
    cur_class = None
    last_alias = None

    def new_section(title):
        nonlocal section
        section = Section(title)
        model.sections.append(section)

    def ensure_section():
        if section is None:
            new_section(fname)

    for raw in lines:
        s = raw.rstrip()
        m = re.match(r'^-- ## (.+)$', s)
        if m:
            new_section(m.group(1))
            doc, tags = [], []
            continue
        if s.startswith('--:'):
            ensure_section()
            section.intro.append(s[3:].strip())
            continue
        m = re.match(r'^-- path (\S+)$', s)
        if m:
            pending_path = m.group(1)
            continue
        if s.startswith('---@meta'):
            continue
        if s.startswith('---|'):
            m = re.match(r"^---\|\s*(\S+)\s*(?:#\s*(.*))?$", s)
            if m and last_alias is not None:
                last_alias.values.append((m.group(1), m.group(2) or ''))
            continue
        if s.startswith('---@'):
            m = re.match(r'^---@(\w+)\s*(.*)$', s)
            tag, rest = m.group(1), m.group(2)
            ensure_section()
            if tag == 'class':
                cm = re.match(r'^(\w+)(?:\s*:\s*(\w+))?', rest)
                name, parent = cm.group(1), cm.group(2)
                cls = model.classes.get(name)
                if cls is None:
                    cls = Cls(name, parent, join_doc(doc))
                    model.classes[name] = cls
                    section.items.append(cls)
                elif doc:
                    cls.desc = join_doc(doc)
                if pending_path:
                    cls.path = pending_path
                    pending_path = None
                cur_class = cls
                doc, tags = [], []
            elif tag == 'alias':
                name, rest2 = split_type(rest)
                typ, desc = split_type(rest2) if rest2 else ('', '')
                a = Alias(name, rest2 if rest2 else '', join_doc(doc))
                model.aliases[name] = a
                section.items.append(a)
                last_alias = a
                doc, tags = [], []
            elif tag == 'field' and cur_class is not None:
                name, rest2 = split_type(rest)
                typ, desc = split_type(rest2)
                cur_class.fields.append(Param(name, typ, desc))
            else:
                tags.append((tag, rest))
            continue
        if s.startswith('---'):
            doc.append(s[3:].strip())
            continue
        if s.startswith('--'):
            continue
        # a code line (or a blank one)
        if s.strip() == '':
            if cur_class is not None and not doc and not tags:
                pass
            # a blank line ends the pending description of anything that is not a declaration
            if tags or doc:
                doc, tags = [], []
            continue
        m = re.match(r'^(?:local\s+)?(\w+)\s*=\s*\{\}\s*$', s)
        if m and cur_class is not None:
            model.var_class[m.group(1)] = cur_class.name
            cur_class = None
            doc, tags = [], []
            continue
        m = re.match(r'^function\s+(\w+)([.:])(\w+)\((.*)\)\s*(?:end)?\s*$', s)
        if m:
            var, sep, name, args = m.groups()
            fn = make_func(var, sep == ':', name, args, doc, tags)
            cls = model.classes.get(model.var_class.get(var, ''))
            if cls is not None:
                fn.owner = cls.name
                cls.funcs.append(fn)
                section.items.append(fn)
            else:
                raise SystemExit(f'{fname}: function {var}{sep}{name}: {var} is not a documented class table')
            doc, tags = [], []
            cur_class = None
            continue
        m = re.match(r'^function\s+(\w+)\((.*)\)\s*(?:end)?\s*$', s)
        if m:
            name, args = m.groups()
            fn = make_func(None, False, name, args, doc, tags)
            model.globals.append(fn)
            section.items.append(fn)
            doc, tags = [], []
            continue
        doc, tags = [], []
        cur_class = None


def make_func(owner, colon, name, args, doc, tags):
    params, returns, overloads = [], [], []
    deprecated = None
    for tag, rest in tags:
        if tag == 'param':
            pn, rest2 = split_type(rest)
            typ, desc = split_type(rest2)
            params.append(Param(pn, typ, desc))
        elif tag == 'return':
            typ, rest2 = split_type(rest)
            rname, desc = '', ''
            if rest2:
                parts = rest2.split(None, 1)
                rname = parts[0]
                desc = parts[1] if len(parts) > 1 else ''
            returns.append((typ, rname, desc))
        elif tag == 'overload':
            overloads.append(rest)
        elif tag == 'deprecated':
            deprecated = rest or 'deprecated'
    return Func(owner, name, colon, join_doc(doc), params, returns, overloads, deprecated, args)
