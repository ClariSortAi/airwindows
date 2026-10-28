"""Exercise the actual native Python HTTPS receiver on ::1 only. No radio or LAN listener."""
from pathlib import Path
import argparse
import hashlib
import ipaddress
import json
import plistlib
import queue
import socket
import ssl
import sys
import tempfile
import threading
import time

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'receiver'))
import engine
import receiver
import sender
import transfer

class Events:
    def __init__(self):
        self.closed=threading.Event()
        self.items=queue.Queue()
        self.incoming=None
        self.accept=True
        self.answered_immediately=0
    def say(self,event,**fields):
        if event=='asked' and self.accept is not None:
            # Answer synchronously before the event emitter returns. This is
            # the former lost-answer race, not a sleep-dependent approximation.
            if not self.incoming.answer(fields['id'],self.accept):
                raise AssertionError('decision slot was not ready before asked event')
            self.answered_immediately+=1
        self.items.put(dict(event=event,**fields))
    def wait(self,event,seconds=10):
        until=time.monotonic()+seconds
        while True:
            left=until-time.monotonic()
            if left<=0:raise TimeoutError('missing event '+event)
            item=self.items.get(timeout=left)
            if item['event']==event:return item

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path)
args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
files=sorted((root/'receiver').glob('*.py'))+[Path(__file__).resolve()]
def hashes():return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
report={'schema':'airwindows.receiver.verification.v1','source_sha256':hashes(),'python':sys.version,
        'tls':ssl.OPENSSL_VERSION,'checks':[],'transfers':[],'wireless_tested':False,'iphone_transfer_verified':False,'success':False}
def check(condition,name):
    if not condition:raise AssertionError(name)
    report['checks'].append(name)
def digest(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
try:
    with tempfile.TemporaryDirectory(prefix='airwindows-receiver-test-') as temporary:
        workspace=Path(temporary);destination=workspace/'received';destination.mkdir();keys=workspace/'keys'
        config=engine.parse_args(['--loopback','--port','0','--keys',str(keys),'--outdir',str(destination),'--name','AirWindows local test'])
        events=Events();app=engine.Engine(config,events);events.incoming=app.incoming
        context=sender.client_context(str(keys))
        app.start()
        try:
            check(events.wait('receiving')['on'],'receiver started')
            server=app.receiving.server;address=server.socket.getsockname()
            check(address[0]=='::1' and address[3]==0,'listener is exactly IPv6 loopback')
            check(server.socket.getsockopt(socket.IPPROTO_IPV6,socket.IPV6_V6ONLY)==1,'IPv4 mapped listener disabled')
            check(server.verify_request(None,('::1',1,0,0)),'loopback peer allowed')
            for peer in [('2001:db8::1',1,0,0),('::ffff:192.0.2.1',1,0,0),('fe80::1',1,0,4),('::1',1,0,4)]:
                check(not server.verify_request(None,peer),'foreign scope/address rejected '+peer[0]+'%'+str(peer[3]))
            # The same predicate on an AWDL-bound object rejects a different
            # interface and unscoped/global peers without opening any interface.
            saved=(server.bound_address,server.bound_index)
            server.bound_address=ipaddress.IPv6Address('fe80::2');server.bound_index=42
            check(server.verify_request(None,('fe80::1',1,0,42)),'scoped link-local peer allowed')
            for peer in [('fe80::1',1,0,41),('fe80::1',1,0,0),('2001:db8::1',1,0,42),('::1',1,0,42)]:
                check(not server.verify_request(None,peer),'non-AWDL peer rejected '+peer[0]+'%'+str(peer[3]))
            server.bound_address,server.bound_index=saved
            def request(path,body=b'',headers=None):
                connection=sender.Connection('::1',app.settings.port,0,context,5)
                try:
                    connection.request('POST',path,body=body,headers=headers or {})
                    response=connection.getresponse();payload=response.read()
                    return response.status,payload
                finally:connection.close()
            status,payload=request('/Discover',plistlib.dumps({}))
            check(status==200 and plistlib.loads(payload)['ReceiverComputerName']=='AirWindows local test','actual HTTPS discovery property list')
            status,_=request('/Upload',b'',{'Content-Type':'application/x-dvzip'})
            check(status==401 and not list(destination.iterdir()),'upload without approved Ask writes nothing')
            for value in [[],{'Files':[1]},{'Items':'wrong'},{'TransferType':[]}]:
                check(request('/Ask',plistlib.dumps(value))[0]==400,'malformed Ask structure rejected '+repr(value))
            check(request('/Ask',b'invalid property list')[0]==400,'malformed Ask bytes rejected')
            check(request('/Discover',b'',{'Content-Length':str(transfer.MAX_METADATA_BYTES+1)})[0]==413,'oversize metadata rejected before body')
            source=workspace/'sources';source.mkdir()
            payloads={'binary.bin':bytes(range(256))*4097,'empty.txt':b'','unicode-é.txt':'AirWindows native HTTPS test\n'.encode()}
            for name,data in payloads.items():(source/name).write_bytes(data)
            outgoing=sender.outgoing([str(source/n) for n in payloads])
            events.accept=False
            check(sender.deliver('::1',app.settings.port,0,context,'Synthetic sender',outgoing)==sender.DECLINED and
                  not list(destination.iterdir()),'declined transfer writes nothing')
            events.accept=True
            check(sender.deliver('::1',app.settings.port,0,context,'Synthetic sender',outgoing)==sender.SENT,'approved multi-file transfer completes')
            stored=events.wait('stored')
            check(set(stored['names'])==set(payloads),'only announced synthetic filenames stored')
            for name in payloads:
                expected=digest(source/name);actual=digest(destination/name)
                check(expected==actual,'received SHA-256 matches '+name)
                report['transfers'].append({'name':name,'bytes':len(payloads[name]),'source_sha256':expected,'received_sha256':actual})
            prior={p.name:digest(p) for p in destination.iterdir()}
            check(sender.deliver('::1',app.settings.port,0,context,'Synthetic sender',outgoing)==sender.SENT,'repeated names transfer completes')
            check(all(digest(destination/name)==h for name,h in prior.items()) and len(list(destination.iterdir()))==6,
                  'repeat transfer keeps originals and chooses distinct names')
            check(events.answered_immediately==3,'synchronous accept and decline decisions are not lost')
        finally:
            app.stop()
        with socket.socket(socket.AF_INET6,socket.SOCK_STREAM) as probe:
            probe.settimeout(1)
            check(probe.connect_ex(('::1',app.settings.port))!=0,'listener closed after stop')
    check(report['source_sha256']==hashes(),'source hashes stable during verification')
    report['success']=True
finally:
    (out/'verification.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print(json.dumps({'success':report['success'],'checks':len(report['checks']),'files_verified':len(report['transfers']),
                  'wireless_tested':False,'iphone_transfer_verified':False}))
