"""Execute the generated recovery script with fake network tools and real file locks."""
import fcntl
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE=Path('/tmp/lan-address-rollback.sh').read_text()
OLD='192.168.1.1,255.255.255.0,192.168.1.2,192.168.1.254,0'

class RollbackTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.tx=self.root/'transaction';self.tx.mkdir();self.bin=self.root/'bin';self.bin.mkdir()
        s=SOURCE.replace('/tmp/fixture',str(self.tx))
        for old,new in [('/mnt/data/local-udx710/lan-address.pending',self.root/'pending'),('/mnt/data/local-udx710/lan-dhcp.state',self.root/'dhcp-state'),('/var/mm/dhcp_config.ini',self.root/'config'),('/home/root/6677/cw52-lan-recovery.sh',self.root/'helper'),('/run/udx710-lan.lock',self.root/'lock')]:s=s.replace(old,str(new))
        (self.tx/'rollback.sh').write_text(s);(self.tx/'dhcp_config.ini.before').write_text('old-config')
        (self.root/'config').write_text('new-config');(self.root/'pending').write_text(str(self.tx));(self.root/'lock').write_text(str(self.tx))
        (self.root/'helper').write_text('#!/bin/sh\nexit 0\n')
        fake='''#!/usr/bin/python3
import os,sys,json
from pathlib import Path
r=Path(os.environ['FIXTURE_ROOT']);tool=Path(sys.argv[0]).name
with (r/'events').open('a') as f:f.write(json.dumps([tool,*sys.argv[1:]])+'\\n')
if tool=='dbus-send':
 if any('SetProperty' in a for a in sys.argv) and os.environ.get('FAIL_SET')=='1':sys.exit(1)
 print(os.environ['OLD_CONFIG'])
if tool=='iptables' and '-C' in sys.argv:sys.exit(1)
'''
        for tool in ['dbus-send','iptables','ip','sync']:
            (self.bin/tool).write_text(fake);(self.bin/tool).chmod(0o700)
        (self.bin/'sleep').write_text('#!/bin/sh\nexit 0\n');(self.bin/'sleep').chmod(0o700)
        self.env={**os.environ,'PATH':str(self.bin)+':'+os.environ['PATH'],'FIXTURE_ROOT':str(self.root),'OLD_CONFIG':OLD}
    def run_rollback(self, **env):
        return subprocess.run(['/bin/sh',str(self.tx/'rollback.sh'),'--now'],env={**self.env,**env},capture_output=True,text=True,timeout=10)
    def test_restores_file_and_releases_only_owned_lock(self):
        (self.root/'dhcp-state').write_text('disabled\n')
        r=self.run_rollback();self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual((self.root/'config').read_text(),'old-config')
        self.assertFalse((self.root/'pending').exists());self.assertFalse((self.root/'lock').exists())
        events=(self.root/'events').read_text();self.assertIn('CW52_LAN_DHCP',events);self.assertNotIn('flush',events)
    def test_confirmed_transaction_is_not_rolled_back(self):
        (self.tx/'committed').write_text('confirmed')
        self.assertEqual(self.run_rollback().returncode,0)
        self.assertEqual((self.root/'config').read_text(),'new-config');self.assertFalse((self.root/'events').exists())
    def test_foreign_pending_transaction_is_not_touched(self):
        (self.root/'pending').write_text('/different/transaction')
        self.assertEqual(self.run_rollback().returncode,0);self.assertFalse((self.root/'events').exists())
    def test_failed_setter_keeps_recovery_armed(self):
        self.assertNotEqual(self.run_rollback(FAIL_SET='1').returncode,0)
        self.assertTrue((self.root/'pending').exists());self.assertTrue((self.root/'lock').exists())
    def test_confirmation_and_timeout_use_same_lock(self):
        with (self.tx/'finalize.lock').open('w') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            p=subprocess.Popen(['/bin/sh',str(self.tx/'rollback.sh'),'--now'],env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            (self.tx/'committed').write_text('confirmed under lock')
            fcntl.flock(lock,fcntl.LOCK_UN)
            _,err=p.communicate(timeout=5);self.assertEqual(p.returncode,0,err)
        self.assertEqual((self.root/'config').read_text(),'new-config');self.assertFalse((self.root/'events').exists())

if __name__=='__main__':unittest.main()
