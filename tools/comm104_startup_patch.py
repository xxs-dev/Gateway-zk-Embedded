"""Identity-pinned, graph-only COMM104 startup correction. Credentials via env."""
import argparse
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import shlex
import time
import zlib
import paramiko

GRAPH = '/opt/modbus-gateway/config/runtime/logic/shuntong_ems_cycle_2kw.active.json'
APP = '/opt/modbus-gateway/config/runtime/apps/mqtt-service.json'
UNIT = 'compute-engine@mqtt-service.service'
NAMES = {'grid_reserve_stop_latch', 'grid_reserve_stop_clear'}

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--reference', type=Path)
    p.add_argument('--apply', action='store_true')
    a = p.parse_args()
    c = paramiko.SSHClient()
    c.load_system_host_keys()
    c.set_missing_host_key_policy(paramiko.RejectPolicy())
    c.connect('10.126.126.11', username='root', password=os.environ['EDGE_PASSWORD'],
              timeout=8, auth_timeout=8, look_for_keys=False, allow_agent=False)
    def run(code):
        _, out, err = c.exec_command('python3 -c ' + shlex.quote(code), timeout=90)
        result = out.read().decode()
        error = err.read().decode()
        if out.channel.recv_exit_status():
            raise RuntimeError(error + result)
        return json.loads(result)
    header = f"""
import pathlib,json,hashlib,subprocess,os,time,shutil
root=pathlib.Path('/opt/modbus-gateway')
assert json.loads((root/'config/runtime/device_identity.json').read_text())['machineCode']=='COMM202600104'
g=pathlib.Path({GRAPH!r});app=pathlib.Path({APP!r});unit={UNIT!r}
assert g.is_file() and not g.is_symlink()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def points():
 result={{}}
 for store,ids in [('ems_virtual',[740100,740104,740105,740106,740107,740108,740113,740114,740116,740129]),('ttySP4',[1201,1202,1211,1212,1216,1230,1399])]:
  for idx in ids:
   result[str(idx)]=subprocess.check_output([str(root/'bin/pointctl'),'get','--shm','gateway_point_store_'+store,'--index',str(idx)],timeout=3).decode().strip()
 return result
"""
    try:
        if not a.apply:
            a.output.mkdir(parents=True, exist_ok=False)
            snapshot = run(header + """
cfg=json.loads(app.read_text())['computeEngine']
assert any(r.get('enabled') and r.get('script',{}).get('graphFile')==str(g) for r in cfg['rules'])
print(json.dumps({'graph':json.loads(g.read_text()),'graphSha':sha(g),'appSha':sha(app),'points':points(),'at':time.time()}))
""")
            before = snapshot.pop('graph')
            reference = json.loads(a.reference.read_text(encoding='utf-8'))
            replacements = {n['id']: n for n in reference['nodes'] if n['id'] in NAMES}
            assert len(replacements) == 2
            after = copy.deepcopy(before)
            for n in after['nodes']:
                if n['id'] in NAMES:
                    ref = replacements[n['id']]
                    key = 'stateOutputIndex' if n['type'] == 'sequence' else 'outputIndex'
                    assert n['parameters'][key] == ref['parameters'][key]
                    for k in ('parameters', 'ports', 'displayName'):
                        n[k] = copy.deepcopy(ref[k])
            after['links'] = [l for l in after['links'] if l['toNodeId'] not in NAMES]
            new_links = [copy.deepcopy(l) for l in reference['links'] if l['toNodeId'] in NAMES]
            for i,l in enumerate(new_links): l['id'] = 'startup_fix_20260913_' + str(i)
            after['links'].extend(new_links)
            assert len(after['nodes']) == len(before['nodes'])
            for old,new in zip(before['nodes'],after['nodes']):
                if old['id'] not in NAMES: assert old == new
            for name,data in [('before.json',before),('candidate.json',after),('snapshot.json',snapshot)]:
                (a.output/name).write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8')
            print(json.dumps({'graphSha':snapshot['graphSha'],'points':snapshot['points'],'output':str(a.output)}))
        else:
            snap=json.loads((a.output/'snapshot.json').read_text())
            payload=base64.b64encode(zlib.compress((a.output/'candidate.json').read_bytes())).decode()
            assert len(payload) < 100000, 'Payload exceeds safe command size'
            code=header+f"""
import base64,zlib
assert sha(g)=={snap['graphSha']!r} and sha(app)=={snap['appSha']!r},'Live configuration changed'
pre=points()
for idx in ['740100','740105']:
 assert pre[idx].split(' value=')[1].split()[0]=={snap['points']['740100'].split(' value=')[1].split()[0]!r},'Mode changed'
backup=root/'backups'/('startup-fix-'+time.strftime('%Y%m%d-%H%M%S'))
backup.mkdir(parents=True,exist_ok=False)
shutil.copy2(g,backup/'graph.json')
data=zlib.decompress(base64.b64decode({payload!r}));json.loads(data)
tmp=g.with_name(g.name+'.startup-new')
def replace(data):
 with tmp.open('wb') as f:f.write(data);f.flush();os.fsync(f.fileno())
 shutil.copymode(g,tmp);os.replace(tmp,g)
 fd=os.open(str(g.parent),os.O_RDONLY);os.fsync(fd);os.close(fd)
try:
 replace(data)
 subprocess.run(['systemctl','restart',unit],check=True,timeout=30)
 time.sleep(8)
 subprocess.run(['systemctl','is-active','--quiet',unit],check=True,timeout=5)
 post=points()
 for idx in ['740105','740116','740129']:
  assert 'quality=1' in post[idx] and 'stale=0' in post[idx],'Invalid postcheck '+idx
 assert sha(app)=={snap['appSha']!r}
 print(json.dumps({{'applied':True,'graphSha':sha(g),'backup':str(backup),'before':pre,'after':post}}))
except BaseException:
 replace((backup/'graph.json').read_bytes())
 subprocess.run(['systemctl','restart',unit],timeout=30)
 raise
"""
            result=run(code)
            (a.output/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
            print(json.dumps(result))
    finally:
        c.close()

if __name__ == '__main__': main()
