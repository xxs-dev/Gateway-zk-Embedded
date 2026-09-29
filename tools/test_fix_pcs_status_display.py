import unittest
from fix_pcs_status_display import rules

class StatusTests(unittest.TestCase):
    def test_states(self):
        routes={i:dict(nodeId='edge-1',tagId=str(i)) for i in [1399,1209,1210,1211,1212]}
        table=rules(routes)
        for bits,want in [((1,0,0),'stopped'),((0,1,0),'standby'),((0,0,1),'running'),
                          ((0,0,0),None),((1,1,0),None),((1,0,1),None)]:
            values={'1399':1,'1212':0,**dict(zip(map(str,[1209,1210,1211]),bits))}
            matches=[r['code'] for r in table if all(str(values[c['tagId']])==c['value'] for c in r['conditions'])]
            self.assertEqual(matches, [] if want is None else [want])

if __name__=='__main__':unittest.main()
