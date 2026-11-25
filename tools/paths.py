"""Where things are in the repository, for every tool: a move changes this file, not the tools.

    import paths
    paths.BILD_ENGINE      # folder with sacred_addr.h and the generated .inc files
    paths.rel(paths.X)     # the same as a repository-relative path with forward slashes (for messages)
"""
import os

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
DATA = os.path.join(TOOLS, 'data')

# The three code roots: the code both DLLs link (sacred_core), ddraw.dll, SacredGear.dll.
CORE = os.path.join(ROOT, 'src', 'core')
BILD = os.path.join(ROOT, 'src', 'bild')
GEAR = os.path.join(ROOT, 'src', 'gear')

# SacredBild
BILD_ENGINE = os.path.join(BILD, 'engine')          # sacred_addr.h, sacred_sigs.inc, gameserver_sigs.inc
SACRED_ADDR = os.path.join(BILD_ENGINE, 'sacred_addr.h')
SACRED_SIGS = os.path.join(BILD_ENGINE, 'sacred_sigs.inc')
GAMESERVER_SIGS = os.path.join(BILD_ENGINE, 'gameserver_sigs.inc')
RESOLUTION_SITES = os.path.join(BILD, 'display', 'resolution_sites.inc')
DDRAW9 = os.path.join(BILD, 'ddraw9')                # the shaders and their compiled .inc files
PROMPTS_PNG = os.path.join(BILD, 'controller', 'prompts.png')

# Sacred Gear
GEAR_ENGINE = os.path.join(GEAR, 'engine')          # gear_addr.h and the generated .inc files
GEAR_ADDR = os.path.join(GEAR_ENGINE, 'gear_addr.h')
GEAR_SIGS = os.path.join(GEAR_ENGINE, 'gear_sigs.inc')
GEAR_SITES = os.path.join(GEAR_ENGINE, 'gear_sites.inc')
GEAR_DROPS = os.path.join(GEAR_ENGINE, 'gear_drops.inc')
GEAR_PFX = os.path.join(GEAR_ENGINE, 'gear_pfx.inc')
GEAR_LUA = os.path.join(GEAR, 'lua')                # kernel.lua and the libraries embedded in the DLL
KERNEL = os.path.join(GEAR_LUA, 'kernel.lua')
LUA_HOST = os.path.join(GEAR, 'lua_host.cpp')
GEAR_META = os.path.join(GEAR, 'meta')
GEAR_MDK_SRC = os.path.join(GEAR, 'mdk')
GEAR_EXAMPLES = os.path.join(GEAR, 'examples')
GEAR_MDK_OUT = os.path.join(ROOT, 'out', 'mdk', 'sacred-gear-mdk')   # generated, not committed

# Documentation
DOCS = os.path.join(ROOT, 'docs')
GEAR_API_MD = os.path.join(DOCS, 'GearApi.md')
GEAR_API_HEAD = os.path.join(DOCS, 'GearApi.head.md')
GEAR_MD = os.path.join(DOCS, 'Gear.md')
GEAR_PFX_TYPES_MD = os.path.join(DOCS, 'GearPfxTypes.md')


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, '/')


def kernel_sources():
    """The kernel's Lua files: kernel.lua and its parts (kernel_*.lua), in a stable order."""
    names = sorted(n for n in os.listdir(GEAR_LUA) if n == 'kernel.lua' or (n.startswith('kernel_') and n.endswith('.lua')))
    return [os.path.join(GEAR_LUA, n) for n in names]


def host_sources():
    """The C++ files that register natives for the kernel: lua_host.cpp and every *_lua.cpp under Gear."""
    out = [LUA_HOST]
    for base, _, files in os.walk(GEAR):
        out += [os.path.join(base, f) for f in sorted(files) if f.endswith('_lua.cpp')]
    return out
