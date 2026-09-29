"""Correct PCS status rules in an existing SCADA project without changing routes."""
import json
from pathlib import Path

def rules(routes):
    def condition(index, value):
        r = routes[index]
        return dict(nodeId=r['nodeId'], tagId=r['tagId'], comparison='eq', value=str(value))
    specs = [('offline', '离线', '#71808A', 100, [(1399, 0)]),
             ('fault', '故障', '#E45858', 90, [(1399, 1), (1212, 1)])]
    for index, code, label, color in [(1209,'stopped','停机','#D9A441'),
                                      (1210,'standby','待机','#39B8D6'),
                                      (1211,'running','运行','#20C879')]:
        conditions = [(1399,1),(1212,0)] + [(i,int(i==index)) for i in (1209,1210,1211)]
        specs.append((code,label,color,50,conditions))
    return [dict(code=code,label=label,color=color,image='',priority=priority,
                 match='all',conditions=[condition(i,v) for i,v in cs])
            for code,label,color,priority,cs in specs]

def patch(root):
    root=Path(root)
    routes={r['index']:r for r in json.loads((root/'runtime-map.json').read_text(encoding='utf-8'))}
    expected=rules(routes)
    changed=[]
    for p in (root/'screens').glob('*.json'):
        doc=json.loads(p.read_text(encoding='utf-8'))
        modified=False
        for w in doc.get('widgets',[]):
            old=w.get('stateRules',[])
            if not any(c.get('tagId')==routes[1211]['tagId'] for r in old for c in r.get('conditions',[])):
                continue
            if not any(r.get('label') in ('停机','待机','运行') for r in old):continue
            w['stateRules']=expected
            w.setdefault('properties',{}).update(defaultStateLabel='未知/状态冲突',defaultStateColor='#71808A')
            changed.append((p.name,w['widgetId']));modified=True
        if modified:p.write_text(json.dumps(doc,ensure_ascii=False,indent=2),encoding='utf-8')
    return changed

if __name__=='__main__':
    import sys
    print(json.dumps(patch(sys.argv[1]),ensure_ascii=False))
