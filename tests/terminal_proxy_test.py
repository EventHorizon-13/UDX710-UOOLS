"""Real TCP proxy lifecycle, authentication and WebSocket byte transport."""
import http.client, socket, subprocess, threading, time
from pathlib import Path
TOKEN='a'*64
Path('/tmp/terminal-proxy-fixture.ini').write_text('gateway_ip=127.0.0.1\n')
server=socket.create_server(('127.0.0.1',17681))
def upstream():
 while True:
  c,_=server.accept()
  def respond(c):
   with c:
    data=b''
    while b'\r\n\r\n' not in data:data+=c.recv(4096)
    assert b'Authorization:' not in data and b'Cookie:' not in data
    if b'GET /ws ' in data:
     c.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n')
     while True:
      b=c.recv(4096)
      if not b:break
      c.sendall(b)
    else:
     body=b'fixture-terminal' if b'GET / ' in data else b'{"token":""}'
     c.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: '+str(len(body)).encode()+b'\r\nConnection: close\r\n\r\n'+body)
  threading.Thread(target=respond,args=(c,),daemon=True).start()
threading.Thread(target=upstream,daemon=True).start()
proc=subprocess.Popen(['/tmp/terminal_proxy_test'])
def request(path,method='GET',headers=None):
 c=http.client.HTTPConnection('127.0.0.1',16677,timeout=3);c.request(method,path,headers=headers or {});r=c.getresponse();result=(r.status,dict(r.getheaders()),r.read());c.close();return result
try:
 time.sleep(.3)
 assert request('/terminal/')[0]==401
 assert request('/api/terminal/session','POST')[0]==401
 assert request('/api/terminal/session','POST',{'Authorization':'Bearer '+TOKEN,'Origin':'https://evil.invalid'})[0]==403
 status,headers,_=request('/api/terminal/session','POST',{'Authorization':'Bearer '+TOKEN})
 assert status==200 and 'HttpOnly' in headers['Set-Cookie'] and TOKEN in headers['Set-Cookie']
 cookie={'Cookie':'cw52_terminal='+TOKEN}
 assert request('/terminal/',headers=cookie)[2]==b'fixture-terminal'
 assert request('/terminal/token',headers=cookie)[0]==200
 assert request('/terminal/ws',headers={**cookie,'Origin':'http://evil.invalid'})[0]==401
 assert request('/terminal/../api/shell',headers=cookie)[0]==404
 assert request('/terminal/?target=evil',headers=cookie)[0]==404
 for i in range(20):
  with socket.create_connection(('127.0.0.1',16677),timeout=3) as c:
   c.sendall(('GET /terminal/ws HTTP/1.1\r\nHost: localhost\r\nCookie: cw52_terminal='+TOKEN+'\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: abc\r\n\r\n').encode())
   data=b''
   while b'\r\n\r\n' not in data:data+=c.recv(4096)
   assert b'101' in data
   payload=b'\x82\x04test';c.sendall(payload);assert c.recv(6)==payload
 assert proc.poll() is None
 print('Terminal HTTP, cookie auth, origin isolation, raw WebSocket and disconnect cycles passed')
finally:
 proc.terminate();proc.wait(timeout=3)
