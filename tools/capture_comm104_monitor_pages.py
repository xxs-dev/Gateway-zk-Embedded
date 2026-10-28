"""One-shot JSON-only monitor snapshots; never navigate/control the live UI."""
import hashlib,json,os,shlex,time,tarfile,html,struct,argparse
from pathlib import Path
import paramiko
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--device',choices=['103','104'],default='104')
parser.add_argument('--all-pages',action='store_true')
args=parser.parse_args()
machine='COMM202600'+args.device
host={'103':'10.126.126.10','104':'10.126.126.11'}[args.device]
out=ROOT/'artifacts'/('comm'+args.device+'-monitor-pages-'+time.strftime('%Y%m%d-%H%M%S'));out.mkdir()
helper=ROOT/'artifacts/comm104-deploy-20260912/capture-json-build-r3/scada_capture_json'
digest='23026a5284fe3c583a9ff85b0c3313fce65b6b1e1506e6d784fb52488eda52b1'
assert hashlib.sha256(helper.read_bytes()).hexdigest()==digest
c=paramiko.SSHClient();c.load_system_host_keys();c.set_missing_host_key_policy(paramiko.RejectPolicy())
c.connect(host,username='root',password=os.environ['EDGE_PASSWORD'],timeout=10,look_for_keys=False,allow_agent=False)
remote='/tmp/'+out.name
def run(cmd):
 _,o,e=c.exec_command(cmd,timeout=120);b=o.read();err=e.read()
 status=o.channel.recv_exit_status()
 if status:raise RuntimeError(str(status)+err.decode(errors='replace')+b.decode(errors='replace'))
 return b
try:
 run('mkdir '+remote)
 s=c.open_sftp();s.get_channel().settimeout(60);s.put(str(helper),remote+'/capture');s.chmod(remote+'/capture',0o755)
 assert run('sha256sum '+remote+'/capture').decode().split()[0]==digest
 code=r'''
import pathlib,json,shutil,urllib.request,urllib.parse,subprocess,time,tarfile,hashlib
root=pathlib.Path('/opt/modbus-gateway');work=pathlib.Path(REMOTE)
assert json.loads((root/'config/runtime/device_identity.json').read_text())['machineCode']==MACHINE
project=root/'scada/current';shutil.copytree(project.resolve(),work/'project')
config=json.loads((root/'config/runtime/apps/monitor-service.json').read_text())
assert not config.get('systemMonitor',{}).get('scadaUpperComputerSafety',{}).get('enabled',False)
tags=json.loads((project/'tags.json').read_text());meters=sorted(set(t['meterCode'] for t in tags if t.get('meterCode')))
points={};times=[]
for meter in meters:
 url='http://127.0.0.1:9443/api/v1/realtime/points?'+urllib.parse.urlencode({'meterCode':meter})
 with urllib.request.urlopen(url,timeout=10) as r:d=json.load(r)
 times.append(d['ts'])
 for p in d['points']:
  p['expireAt']=p.get('expireAt',0);points[p['index']]=p
(work/'samples.json').write_text(json.dumps({'sampleTimestampMs':max(times),'points':list(points.values())}))
p=subprocess.run([str(work/'capture'),'--project',str(work/'project'),'--samples',str(work/'samples.json'),'--output',str(work/'pages')],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=90)
assert p.returncode in (0,2),p.stderr.decode(errors='replace')
report=json.loads((work/'pages/capture-report.json').read_text())
assert report['writeAttempts']==0 and report['liveDataAccess']==False
selected=[p for p in report['pages'] if ALLPAGES or p['screenId']=='Overview' or p['screenId']=='Alarms' or p['screenId'].startswith(('Devices-','Trends-'))]
report['pages']=selected;report['sourceTimeMinMs']=min(times);report['sourceTimeMaxMs']=max(times)
report['deployedProject']=str(project.resolve())
(work/'report.json').write_text(json.dumps(report))
with tarfile.open(work/'monitor.tar.gz','w:gz') as t:
 t.add(work/'report.json',arcname='report.json')
 for page in selected:t.add(work/'pages'/(page['screenId']+'.png'),arcname=page['screenId']+'.png')
print(hashlib.sha256((work/'monitor.tar.gz').read_bytes()).hexdigest())
'''.replace('REMOTE',repr(remote)).replace('MACHINE',repr(machine)).replace('ALLPAGES',repr(args.all_pages))
 expected=run('python3 -c '+shlex.quote(code)).decode().strip()
 s.close()
 (out/'monitor.tar.gz').write_bytes(run('cat '+remote+'/monitor.tar.gz'))
 assert hashlib.sha256((out/'monitor.tar.gz').read_bytes()).hexdigest()==expected
 with tarfile.open(out/'monitor.tar.gz') as t:
  for m in t.getmembers():
   assert m.isfile() and '/' not in m.name and '\\' not in m.name
   (out/m.name).write_bytes(t.extractfile(m).read())
 report=json.loads((out/'report.json').read_text());names=[]
 for p in report['pages']:
  f=out/(p['screenId']+'.png');assert hashlib.sha256(f.read_bytes()).hexdigest()==p['pngSha256']
  assert struct.unpack('>II',f.read_bytes()[16:24])==(p['width'],p['height'])
  names.append(p['screenId'])
 labels={'FirstPage':'首页','MainPage_2':'主画面','RunMon':'运行监测','PcsMon':'PCS监测',
         'BmsMon':'BMS监测','BmsRun':'BMS运行','FireFight':'消防监测','IoMon':'开关量监测',
         'UpsMon':'辅助设备监测','DataTable':'数据表','PreAlarm':'告警',
         'ChargeParam':'充电参数','DevParam':'设备参数','RunParam':'运行参数',
         'RunMode':'运行模式','LocalControl':'本地控制'}
 body=''.join('<h2>'+html.escape(labels.get(n,n))+' ('+html.escape(n)+')</h2><img loading="lazy" src="'+n+'.png">' for n in names)
 (out/'index.html').write_text('<!doctype html><meta charset="utf-8"><title>'+machine+'</title><style>body{background:#202428;color:white;font:16px sans-serif;margin:24px}img{width:100%;max-width:1920px}</style><h1>'+machine+' 页面快照</h1><p>当前部署画面 + 本次实时数据采样的离屏渲染。不是逐页物理屏幕截图；趋势历史未装载，空曲线不能用于判断现场历史丢失。未执行控制操作。</p>'+body,encoding='utf-8')
 print(json.dumps({'output':str(out),'pages':names},ensure_ascii=False))
finally:c.close()
