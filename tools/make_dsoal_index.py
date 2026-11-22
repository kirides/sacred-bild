#!/usr/bin/env python3
"""Write sacred-dsoal.json: the sibling index (next to mod-index.json) with the DSOAL plugins for the Gear Mod Manager.

mod-index.json refers to it with a relative url. It relays to the official kcat/dsoal archive release (nothing is rebuilt or redistributed here):

    sacred-dsoal             dsound.dll + dsoal-aldrv.dll (Win32) from the official zip
    sacred-dsoal-headphones  the official DSOAL+HRTF alsoft.ini from the same zip
    sacred-dsoal-speakers    docs/dsoal/alsoft-speakers.ini of this repository as a loose file, pinned by sha256

    python tools/make_dsoal_index.py            # rewrite sacred-dsoal.json
    python tools/make_dsoal_index.py --check    # CI: exit 1 with a diff when the file is out of date
"""
import argparse, difflib, hashlib, json, os, sys

REPOSITORY = 'kirides/sacred-bild'
SPEAKERS_INI = 'docs/dsoal/alsoft-speakers.ini'
# The official release asset. It holds one more zip (DSOAL_r697.zip) with the folders DSOAL and DSOAL+HRTF (Win32 and Win64 files); 'inner' names it.
OFFICIAL_URL = 'https://github.com/kcat/dsoal/releases/download/archive/DSOAL_r697.zip'
OFFICIAL_SHA256 = '1f7f6b6627290af5c8c882c61ab3461cba1f12d369e155affe006d6919858481'
REVISION = 'r697'
AUTHOR = 'DSOAL and OpenAL Soft contributors'


def sha256_file(path):
    with open(path, 'rb') as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def build(repo):
    ini = os.path.join(repo, SPEAKERS_INI)
    speakers_url = 'https://raw.githubusercontent.com/%s/main/%s' % (REPOSITORY, SPEAKERS_INI)
    common = {'kind': 'plugin', 'platforms': ['win'], 'version': REVISION, 'author': AUTHOR, 'tags': ['audio']}
    return {
        'schema': 1,
        'name': 'Sacred DSOAL',
        'author': 'kirides',
        'description': 'Positional 3D sound and EAX-style reverb for Sacred Gold through DSOAL and OpenAL Soft (official kcat/dsoal release), with a Speakers or Headphones (HRTF) setting. See docs/Dsoal.md.',
        'homepage': 'https://github.com/%s/blob/main/docs/Dsoal.md' % REPOSITORY,
        'download': {'url': OFFICIAL_URL, 'sha256': OFFICIAL_SHA256, 'version': REVISION, 'inner': 'DSOAL_r697.zip'},
        'mods': [
            dict(common, id='sacred-dsoal', path='DSOAL/Win32', name='sacred-dsoal (base)',
                 description='Switches on the 3D audio Sacred was built for: positional 3D sound and EAX-style reverb. DSOAL (dsound.dll) presents a hardware DirectSound3D/EAX device to the game '
                             'and OpenAL Soft (dsoal-aldrv.dll) mixes on any sound card. Installs only the two DLLs; pick Speakers or Headphones for the output setting. '
                             'Needs the Microsoft Visual C++ 2015-2022 Redistributable (x86). After installing, choose "3D, alle Effekte" in the sound options and restart the game.',
                 install={'files': ['dsound.dll', 'dsoal-aldrv.dll']}),
            dict(common, id='sacred-dsoal-headphones', path='DSOAL+HRTF/Win32', name='DSOAL Headphones (alsoft.ini, HRTF)', requires=['sacred-dsoal'],
                 description='Audio setting for headphones: stereo output with HRTF spatialisation (the official DSOAL+HRTF alsoft.ini: channels=stereo, stereo-mode=headphones, stereo-encoding=hrtf). '
                             'Installs only an alsoft.ini. Installs the base plugin first; replaces the Speakers setting.',
                 install={'files': ['alsoft.ini']}),
            dict(common, id='sacred-dsoal-speakers', name='DSOAL Speakers (alsoft.ini)', requires=['sacred-dsoal'],
                 description='Audio setting for speakers: stereo output is played as speakers with plain panning and no HRTF; the speaker layout comes from the Windows sound settings. '
                             'Installs only an alsoft.ini. Installs the base plugin first; replaces the Headphones setting.',
                 install={'files': [{'url': speakers_url, 'sha256': sha256_file(ini), 'to': 'alsoft.ini'}]}),
        ],
    }


def main(argv=None):
    a = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    a.add_argument('--repo', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    a.add_argument('--check', action='store_true', help='write nothing; exit 1 when the file differs')
    o = a.parse_args(argv)
    out = os.path.join(o.repo, 'sacred-dsoal.json')
    text = json.dumps(build(o.repo), indent=2, ensure_ascii=False) + '\n'
    old = ''
    if os.path.exists(out):
        with open(out, encoding='utf-8-sig', newline='') as fh:
            old = fh.read().replace('\r\n', '\n')
    if o.check:
        if text == old:
            return 0
        sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), text.splitlines(True), 'sacred-dsoal.json (on disk)', 'sacred-dsoal.json (generated)'))
        print('sacred-dsoal.json is out of date: run python tools/make_dsoal_index.py', file=sys.stderr)
        return 1
    if text != old:
        with open(out, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
    print('sacred-dsoal.json: %s' % ', '.join(m['id'] for m in json.loads(text)['mods']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
