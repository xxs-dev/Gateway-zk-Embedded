"""Capture local source/build identity without reading runtime secrets."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[3]
here = Path(__file__).resolve().parent
build = Path('/tmp/gateway-fixed-cluster-7e392ce-20260924')
paths = ['CMakeLists.txt', 'tools/ems_cluster_test.cpp',
         'tools/graph_ems_cluster_dispatch_test.cpp', 'src/ems_cluster.cpp',
         'src/ems_cluster_dispatch.cpp', 'src/ems_cluster_transport.cpp',
         'src/ems_cluster_network.cpp', 'src/ems_cluster_points.cpp',
         'src/graph_ems_engine.cpp', 'src/memory_point_store.cpp',
         'src/point_store_router.cpp', 'src/config_loader.cpp']


def output(args):
    if args[0] == 'git' and os.name != 'nt':
        args = ['git', '--git-dir=/mnt/d/workspace/Embedded/Gateway-zk/.git/worktrees/cluster-evidence-20260924',
                '--work-tree=' + str(root), *args[1:]]
    return subprocess.check_output(args, text=True, cwd=root).strip()


result = {
    'sourceCommit': output(['git', 'rev-parse', 'HEAD']),
    'trackedDiff': output(['git', 'diff', 'HEAD', '--', '.', ':!evidence']),
    'capturePlatform': os.name,
    'compilerEvidence': 'fixed-configure.log identifies the native GNU compiler',
    'sourceSha256': {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths},
    'clusterCasesCalledInMain': re.findall(r'^\s+(test\w+)\(\);',
                                          (root / paths[1]).read_text(), re.M),
    'postRunBuildDirectoryMissing': True,
    'artifactRetentionNote': 'The WSL /tmp build directory was absent on a later call. '
                            'Binary SHA256 was captured immediately after each successful run. '
                            'Library/cache could not be collected afterward; no hash is invented.',
}
assert result['sourceCommit'] == '7e392cec6218244b0a230a5f0d58c96888fd51de'
assert not result['trackedDiff']
assert len(result['clusterCasesCalledInMain']) == 23
(here / 'fixed-provenance.json').write_text(json.dumps(result, indent=2) + '\n')
print('Fixed source verified; 23 cluster cases; provenance saved.')
