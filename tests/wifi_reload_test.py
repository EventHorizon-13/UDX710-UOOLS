"""Exercise the real adapter with isolated process/CLI fixtures, never real signals."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WifiReloadTest(unittest.TestCase):
    def run_adapter(self, interface, command, *, pids='123', exe=None, signal_status=0):
        if exe is None:
            exe = '/usr/sbin/hostapd' if interface == 'wlan1' else '/usr/sbin/hostapd_5g'
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            log = directory / 'events'
            cli = directory / 'vendor-cli'
            cli.write_text('#!/bin/sh\nprintf "cli" >> "$EVENT_LOG"\n'
                           'for arg do printf " <%s>" "$arg" >> "$EVENT_LOG"; done\n'
                           'printf "\\n" >> "$EVENT_LOG"\nprintf "vendor-result\\n"\n')
            cli.chmod(0o700)
            adapter = (ROOT / 'cw52-hostapd-cli.sh').read_text().replace(
                'CLI=/usr/sbin/hostapd_cli', 'CLI=' + shlex.quote(str(cli)))
            mocks = '''pidof() { printf '%s' "$FIXTURE_PIDS"; }
readlink() { printf '%s' "$FIXTURE_EXE"; }
kill() { printf 'signal %s %s\\n' "$1" "$2" >> "$EVENT_LOG"; return "$SIGNAL_STATUS"; }
sleep() { printf 'sleep %s\\n' "$1" >> "$EVENT_LOG"; }
'''
            script = directory / 'adapter'
            script.write_text(mocks + adapter)
            env = {**os.environ, 'EVENT_LOG': str(log), 'FIXTURE_PIDS': pids,
                   'FIXTURE_EXE': exe, 'SIGNAL_STATUS': str(signal_status)}
            result = subprocess.run(['/bin/sh', str(script), '-p', '/var/run/hostapd',
                                     '-i', interface, command], env=env,
                                    capture_output=True, text=True, timeout=5)
            return result, log.read_text().splitlines() if log.exists() else []

    def test_reload_signals_only_matching_radio(self):
        for interface in ('wlan1', 'wlan0'):
            with self.subTest(interface=interface):
                result, events = self.run_adapter(interface, 'reload')
                self.assertEqual((result.returncode, result.stdout), (0, 'OK\n'))
                self.assertEqual(events, ['signal -HUP 123'])

    def test_enable_reads_disk_before_vendor_enable(self):
        result, events = self.run_adapter('wlan1', 'enable')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(events[:2], ['signal -HUP 123', 'sleep 1'])
        self.assertEqual(events[2], 'cli <-p> </var/run/hostapd> <-i> <wlan1> <enable>')

    def test_read_and_disable_delegate_without_signal(self):
        for command in ('status', 'all_sta', 'disable'):
            with self.subTest(command=command):
                result, events = self.run_adapter('wlan0', command)
                self.assertEqual(result.stdout, 'vendor-result\n')
                self.assertEqual(events, [f'cli <-p> </var/run/hostapd> <-i> <wlan0> <{command}>'])

    def test_unidentified_process_is_rejected_without_signal(self):
        for fixture in ({'pids': ''}, {'pids': '123 456'},
                        {'exe': '/usr/sbin/other'}, {'pids': '123;false'}):
            with self.subTest(fixture=fixture):
                result, events = self.run_adapter('wlan1', 'reload', **fixture)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(events, [])

    def test_signal_failure_cannot_report_success(self):
        result, events = self.run_adapter('wlan1', 'reload', signal_status=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, 'FAIL\n')
        self.assertEqual(events, ['signal -HUP 123'])

    def test_unknown_radio_cannot_signal_process(self):
        result, events = self.run_adapter('wlan9', 'reload')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(events, [])


if __name__ == '__main__':
    unittest.main()
