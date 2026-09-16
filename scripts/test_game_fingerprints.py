#!/usr/bin/env python3
"""Check which real staged ingredients invalidate each title's cached game."""
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fingerprints', ROOT / 'packaging/linux/game_fingerprints.py')
builds = importlib.util.module_from_spec(spec); spec.loader.exec_module(builds)


class FingerprintTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='game fingerprint checks ')
        self.app = Path(self.tmp.name) / 'AppDir'
        self.resource = self.app / 'usr/share/last-raven'
        self.abis = dict(SDL2='libSDL2-2.0.so.0', openh264='libopenh264.so.8',
                         avcodec='libavcodec.so.63', avutil='libavutil.so.61')
        self.profiles = json.loads((ROOT / 'packaging/linux/games.json').read_text())
        self.write('usr/share/last-raven/games.json', json.dumps(self.profiles))
        for path in ('usr/share/last-raven/libruntime.a', 'usr/share/last-raven/include/shared.h',
                     'usr/share/last-raven/compile_game.py', 'usr/share/last-raven/emit-split.py', 'usr/share/last-raven/fps-loop.py',
                     'usr/zig/zig', 'usr/zig/lib/std.zig', 'usr/bin/allegrexrecomp'):
            self.write(path, 'original ' + path)
        self.write('usr/share/last-raven/host/common.h', '#include "nested.h"\n')
        self.write('usr/share/last-raven/host/nested.h', '#define COMMON 1\n')
        self.write('usr/share/last-raven/host/ac3_controls.h', '#include "common.h"\n')
        for profile in self.profiles:
            slug = profile['slug']
            self.write(f'usr/share/last-raven/libhost-{slug}.a', 'host ' + slug)
            header = 'common.h' if slug == 'aclr' else 'ac3_controls.h'
            self.write('usr/share/last-raven/host/' + profile['replacements'],
                       f'#include "{header}"\n#include "{slug}_funcs.h"\n')
            self.write('usr/share/last-raven/host/' + profile['replace_list'], '00102018\n')
        self.baseline = self.ids()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, text):
        path = self.app / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text)

    def ids(self):
        return {slug: entry['id'] for slug, entry in builds.fingerprints(self.app, self.abis)['games'].items()}

    def changed(self):
        return {slug for slug, value in self.ids().items() if value != self.baseline[slug]}

    def test_launcher_and_packaging_changes_preserve_all_games(self):
        for path in ('usr/bin/launcher', 'usr/bin/import-game', 'usr/bin/run-game',
                     'usr/share/last-raven/import_game.py', 'usr/share/last-raven/build-id',
                     'usr/share/last-raven/DejaVuSans.ttf', 'last-raven.svg', 'README.txt',
                     'licenses/THIRD-PARTY.txt', 'BUILD.json'):
            self.write(path, 'updated app UI or packaging')
        self.assertEqual(self.changed(), set())

    def test_display_names_and_tab_order_preserve_all_games(self):
        for profile in self.profiles:
            profile['title'] += ' new display label'
        self.write('usr/share/last-raven/games.json', json.dumps(list(reversed(self.profiles)), indent=4))
        self.assertEqual(self.changed(), set())

    def test_title_replacement_and_replace_list_are_independent(self):
        for profile in self.profiles:
            for field in ('replacements', 'replace_list'):
                path = self.resource / 'host' / profile[field]; original = path.read_text()
                path.write_text(original + '\n/* changed */\n')
                self.assertEqual(self.changed(), {profile['slug']})
                path.write_text(original)

    def test_title_host_and_supported_executable_are_independent(self):
        host = self.resource / 'libhost-acsl.a'; host.write_text('updated title host')
        self.assertEqual(self.changed(), {'acsl'})
        self.baseline = self.ids()
        self.profiles[0]['elf_sha256'] = 'f' * 64
        self.write('usr/share/last-raven/games.json', json.dumps(self.profiles))
        self.assertEqual(self.changed(), {'ac3p'})

    def test_shared_sibling_controls_affect_only_the_siblings(self):
        self.write('usr/share/last-raven/host/ac3_controls.h', '#include "common.h"\n// changed\n')
        self.assertEqual(self.changed(), {'ac3p', 'acsl'})

    def test_transitive_shared_header_affects_every_consumer(self):
        self.write('usr/share/last-raven/host/nested.h', '#define COMMON 2\n')
        self.assertEqual(self.changed(), {'ac3p', 'acsl', 'aclr'})

    def test_runtime_codegen_compiler_and_recipe_changes_invalidate_all(self):
        for path in ('usr/share/last-raven/libruntime.a', 'usr/share/last-raven/include/shared.h',
                     'usr/bin/allegrexrecomp', 'usr/zig/zig', 'usr/zig/lib/std.zig',
                     'usr/share/last-raven/compile_game.py', 'usr/share/last-raven/emit-split.py', 'usr/share/last-raven/fps-loop.py'):
            file = self.app / path; original = file.read_text(); file.write_text('new build ingredient')
            self.assertEqual(self.changed(), {'ac3p', 'acsl', 'aclr'}, path)
            file.write_text(original)

    def test_compatible_shared_library_update_needs_no_relink(self):
        self.write('usr/lib/libavcodec.so.63', 'new library implementation with same ABI')
        self.assertEqual(self.changed(), set())
        self.abis['avcodec'] = 'libavcodec.so.64'
        self.assertEqual(self.changed(), {'ac3p', 'acsl', 'aclr'})

    def test_app_relocation_preserves_ids(self):
        moved = self.app.with_name('new app location with spaces')
        shutil.move(self.app, moved); self.app = moved
        self.assertEqual(self.ids(), self.baseline)

    def test_unknown_quoted_include_fails_closed(self):
        self.write('usr/share/last-raven/host/replacements.c', '#include "missing-game-input.h"\n')
        with self.assertRaisesRegex(ValueError, 'Untracked replacement include'):
            self.ids()


if __name__ == '__main__':
    unittest.main(verbosity=2)
