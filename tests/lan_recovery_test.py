"""Run the actual boot helper against an isolated ip fixture, with no network changes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HELPER=Path(__file__).resolve().parents[1]/'cw52-lan-recovery.sh'

class RecoveryTest(unittest.TestCase):
    def run_helper(self, addresses, fail=False):
        with tempfile.TemporaryDirectory() as folder:
            d=Path(folder);(d/'addresses').write_text(addresses)
            (d/'ip').write_text('''#!/usr/bin/python3
import os,sys
from pathlib import Path
p=Path(os.environ['FIXTURE_DIR'])
if sys.argv[1:]==['-4','-o','address','show','dev','tether']:
 print((p/'addresses').read_text(),end='')
elif sys.argv[1:]==['-4','address','add','192.168.1.1/24','dev','tether']:
 with (p/'events').open('a') as f:f.write('add-maintenance-only\\n')
 if os.environ['FIXTURE_FAIL']=='1':sys.exit(1)
 with (p/'addresses').open('a') as f:f.write('37: tether inet 192.168.1.1/24 scope global tether\\n')
else:sys.exit(97)
''');(d/'ip').chmod(0o700)
            r=subprocess.run(['/bin/sh',str(HELPER)],env={**os.environ,'PATH':str(d)+':'+os.environ['PATH'],'FIXTURE_DIR':str(d),'FIXTURE_FAIL':str(int(fail))},capture_output=True,text=True,timeout=5)
            return r.returncode,(d/'addresses').read_text(),(d/'events').read_text() if (d/'events').exists() else ''
    def test_existing_maintenance_address_is_untouched(self):
        code,addresses,events=self.run_helper('37: tether inet 192.168.1.1/25 scope global tether\n')
        self.assertEqual(code,0);self.assertEqual(events,'')
    def test_add_alias_without_removing_primary(self):
        original='37: tether inet 10.0.0.52/24 scope global tether\n'
        code,addresses,events=self.run_helper(original)
        self.assertEqual(code,0);self.assertTrue(addresses.startswith(original));self.assertIn('192.168.1.1/24',addresses);self.assertEqual(events,'add-maintenance-only\n')
    def test_failed_alias_is_reported(self):
        code,_,_=self.run_helper('37: tether inet 10.0.0.52/24 scope global tether\n',True)
        self.assertNotEqual(code,0)
    def test_unconfigured_bridge_is_untouched(self):
        code,addresses,events=self.run_helper('')
        self.assertNotEqual(code,0);self.assertEqual(events,'')

if __name__=='__main__':unittest.main()
