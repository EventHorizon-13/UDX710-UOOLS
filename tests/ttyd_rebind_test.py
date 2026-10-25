"""Exercise terminal rebinding with real TCP listeners and owned processes."""
import os, signal, socket, subprocess, tempfile, unittest
from pathlib import Path

FAKE = r'''
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
int main(int n,char **v) {
 const char *ip=0; int port=0;
 for(int i=1;i+1<n;i++) {if(!strcmp(v[i],"-i"))ip=v[++i];else if(!strcmp(v[i],"-p"))port=atoi(v[++i]);}
 if(!ip||!port)return 1;
 int s=socket(AF_INET,SOCK_STREAM,0),one=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
 struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)};
 if(inet_pton(AF_INET,ip,&a.sin_addr)!=1||bind(s,(void*)&a,sizeof(a))||listen(s,8))return 2;
 for(;;) {int c=accept(s,0,0);if(c<0)continue;char b[2048];read(c,b,sizeof(b));
 const char *r="HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
 write(c,r,strlen(r));close(c);}
}
'''

class RebindTest(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
  self.root=Path(self.tmp.name);self.d=self.root/'terminal';self.d.mkdir();self.run=self.root/'run';self.run.mkdir()
  c=self.root/'fake.c';c.write_text(FAKE)
  subprocess.run(['gcc','-Wall','-Wextra','-Werror',str(c),'-o',str(self.d/'ttyd')],check=True)
  with socket.socket() as s:s.bind(('127.0.0.1',0));self.port=s.getsockname()[1]
  self.config=self.root/'config';self.config.write_text('gateway_ip=127.0.0.2\n')
  self.pidfile=self.run/('local-ttyd-%s.pid'%self.port)
  source=Path('cw52-ttyd-rebind.sh').read_text().replace('D=/mnt/data/local-ttyd','D='+str(self.d)).replace('CONFIG=/var/mm/dhcp_config.ini','CONFIG='+str(self.config)).replace('RUN=/run','RUN='+str(self.run)).replace('PORT=7681','PORT='+str(self.port))
  self.helper=self.root/'helper.sh';self.helper.write_text(source)
  self.addCleanup(self.clean_process)
 def clean_process(self):
  if self.pidfile.exists():
   try:os.kill(int(self.pidfile.read_text()),signal.SIGTERM)
   except (ProcessLookupError,ValueError):pass
 def invoke(self):
  return subprocess.run(['sh',str(self.helper)],capture_output=True,text=True,timeout=15)
 def pid(self):return int(self.pidfile.read_text())
 def test_initial_start_rebind_noop_and_rollback(self):
  self.assertEqual(self.invoke().returncode,0);first=self.pid()
  self.assertEqual(self.invoke().returncode,0);self.assertEqual(self.pid(),first)
  self.config.write_text('gateway_ip=127.0.0.3\n')
  self.assertEqual(self.invoke().returncode,0);self.assertNotEqual(self.pid(),first)
  with socket.create_connection(('127.0.0.3',self.port),timeout=2) as s:s.sendall(b'GET / HTTP/1.0\r\n\r\n');s.recv(4096)
  self.config.write_text('gateway_ip=127.0.0.2\n')
  self.assertEqual(self.invoke().returncode,0)
  with socket.create_connection(('127.0.0.2',self.port),timeout=2) as s:s.sendall(b'GET / HTTP/1.0\r\n\r\n');s.recv(4096)
 def test_does_not_kill_foreign_process(self):
  p=subprocess.Popen(['sleep','30'])
  try:
   self.pidfile.write_text(str(p.pid))
   self.assertNotEqual(self.invoke().returncode,0);self.assertIsNone(p.poll())
  finally:
   self.pidfile.unlink();p.terminate();p.wait()
 def test_rejects_disabled_and_invalid_addresses(self):
  (self.d/'disabled').touch();self.assertNotEqual(self.invoke().returncode,0);(self.d/'disabled').unlink()
  for ip in ['127.00.0.2','127.0.0.256','127.0.0.2;touch BAD','127.0.0.2\ngateway_ip=127.0.0.3']:
   self.config.write_text('gateway_ip='+ip+'\n');self.assertNotEqual(self.invoke().returncode,0)
  self.assertFalse(self.pidfile.exists())
 def test_failed_bind_can_be_restored(self):
  self.assertEqual(self.invoke().returncode,0)
  with socket.socket() as occupied:
   occupied.bind(('127.0.0.3',self.port));occupied.listen()
   self.config.write_text('gateway_ip=127.0.0.3\n');self.assertNotEqual(self.invoke().returncode,0)
   self.config.write_text('gateway_ip=127.0.0.2\n');self.assertEqual(self.invoke().returncode,0)
 def test_rejects_foreign_lock_and_unsafe_pid(self):
  lock=self.run/('local-ttyd-%s.lock'%self.port);lock.mkdir()
  self.assertNotEqual(self.invoke().returncode,0);lock.rmdir()
  self.pidfile.write_text('1');self.assertNotEqual(self.invoke().returncode,0);self.pidfile.unlink()

if __name__=='__main__':unittest.main()
