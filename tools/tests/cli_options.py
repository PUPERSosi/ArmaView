"""Command-line options of the viewer that act on the model (qt-req 1.2): each must take effect although the model is
read in the background after the options are parsed, and --screenshot must wait until the model is applied.

Usage: python tools/tests/cli_options.py <ArmaView.exe> <Arma 3 Samples Addons folder> [<work folder>]
Each case starts the viewer with a model + one option and a ui-script that waits until everything is loaded and
writes the viewer's "optstate". Exit code 1 when a case fails. Files only in the work folder (default: a temp folder).
"""
import os, subprocess, sys, tempfile

exe, samples = os.path.abspath(sys.argv[1]), sys.argv[2]
work = sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp(prefix='av_cli_options_')
os.makedirs(work, exist_ok=True)
tank = os.path.join(samples, 'Test_Tank_01', 'Test_Tank_01.p3d')
man = os.path.join(samples, 'Test_Character_01', 'A3_character_example.p3d')
rtm = os.path.join(samples, 'Test_Anims', 'AmovPercMstpSrasWrflDnon.rtm')

def state(files, opts, name, steps=''):
    script = os.path.join(work, name + '_script.txt')
    report = name + '_report.txt'
    with open(script, 'w', encoding='utf-8') as f:
        f.write('wait_config\nwait 10\n' + steps + 'optstate\nreport ' + report + '\nexit\n')
    subprocess.run([exe] + files + opts + ['--ui-script', script], timeout=240)
    text = open(os.path.join(work, report), encoding='utf-8', errors='replace').read()
    line = next((l for l in text.splitlines() if l.startswith('optstate')), '')
    kv = dict(p.split('=', 1) for p in line.split(': ', 1)[-1].split() if '=' in p)
    return kv, line

cases = [
    ('lod', [tank], ['--lod', '1'], lambda s: s.get('lod') == '1'),
    ('select', [tank], ['--select', 'OtocVez'], lambda s: s.get('selection', '').lower() == 'otocvez'),
    ('isolate', [tank], ['--isolate', 'OtocVez'], lambda s: s.get('isolate', '').lower() == 'otocvez' and int(s.get('hiddenFaces', '0')) > 0),
    ('highlight', [tank], ['--highlight', 'bones4'], lambda s: s.get('highlight') == '2'),
    ('focus-bone', [man], ['--focus-bone', 'head'], lambda s: s.get('bone', '').lower() == 'head'),
    ('compare-rtm', [man], ['--compare-rtm', rtm], lambda s: int(s.get('compare', '0/0').split('/')[0]) > 10),
    ('section', [tank], ['--section', '2'], lambda s: s.get('section') == '2'),
    ('two models + lod', [tank, man], ['--lod', '1'], lambda s: s.get('model', '').lower() == 'a3_character_example.p3d' and s.get('lod') == '1'),
    # B of the compare follows the model: bound to the tank (no matching bones), then the character is opened -
    # bound to its skeleton then
    ('compare-rtm after a model change', [tank], ['--compare-rtm', rtm], lambda s: s.get('model', '').lower() == 'a3_character_example.p3d'
     and int(s.get('compare', '0/0').split('/')[0]) > 10, 'open ' + man + '\nwait_config\nwait 5\n'),
]
failed = 0
for name, files, opts, ok, *steps in cases:
    s, line = state(files, opts, name.replace(' ', '_').replace('+', ''), steps[0] if steps else '')
    good = bool(line) and ok(s)
    failed += 0 if good else 1
    print(f"{'ok  ' if good else 'FAIL'} {name}: {line or 'no optstate in the report'}")

# --screenshot: taken after the last model is applied (the log has the model before the screenshot), the app exits
shot = os.path.join(work, 'shot.png')
if os.path.exists(shot): os.remove(shot)
log = os.path.join(os.path.dirname(exe), 'ArmaView.log')
subprocess.run([exe, tank, man, '--lod', '1', '--screenshot', shot], timeout=240)
text = open(log, encoding='utf-8', errors='replace').read() if os.path.exists(log) else ''
saved = [l for l in text.splitlines() if 'Screenshot saved' in l]
last = saved[-1] if saved else ''
# taken with the last model applied at LOD 1 (the viewer says what was on screen)
good = os.path.exists(shot) and 'model A3_character_example.p3d, LOD 1' in last
failed += 0 if good else 1
print(f"{'ok  ' if good else 'FAIL'} screenshot: file {'written' if os.path.exists(shot) else 'missing'}; {last.strip() or 'no Screenshot saved line in the log'}")
print(f"{failed} failed")
sys.exit(1 if failed else 0)
