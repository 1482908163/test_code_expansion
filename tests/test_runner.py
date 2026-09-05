#!/usr/bin/env python3
"""Verify failure continuation and warmup filtering with a mocked mesh program."""
import csv
import os
import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    launcher = tmp/'launcher'
    launcher.write_text('#!/bin/bash\nshift 2\nexec "$@"\n')
    launcher.chmod(0o755)
    binary = tmp/'mesh'
    binary.write_text('''#!/usr/bin/env python3
import json,sys,pathlib
args=sys.argv
value=lambda name: args[args.index(name)+1]
algorithm=value('--algorithm')
repeat=int(value('--profile-repeat'))
if algorithm=='baseline' and repeat==1: sys.exit(9)
row={'rank':0,'ranks':1,'repeat':repeat,'experiment':algorithm,
     'metadata':{'algorithm':algorithm,'timing_mode':'natural','core_only':'true'},
     'metrics':{'core_seconds':1,'local_volume_elements_before_adjacency':10},
     'stages':{'face_pipeline_total':{'seconds':0.1,'calls':1}}}
out=pathlib.Path(value('--profile-dir'))
(out/'rank_profiles.jsonl').write_text(json.dumps(row)+'\\n')
''')
    binary.chmod(0o755)
    source = tmp/'input.step'
    source.write_text('mock')
    env = os.environ | dict(LOAD_MODULES='0', CLUSTER_ENV_STRICT='0', PROCESS_COUNT='1',
        RUN_ROOT=str(tmp/'results'), BINARY=str(binary), INPUT_PATH=str(source),
        ALGORITHMS='baseline sparse', TIMING_MODES='natural', REPEATS='1', WARMUPS='1',
        MPI_LAUNCHER=str(launcher), MPI_EXTRA_ARGS=' ', START_EPOCH='0', VERIFY_FACES='0',
        RANKS_PER_NODE='1', TIMEOUT_SECONDS='10')
    result = subprocess.run(['bash', str(ROOT/'strong_scaling/run_experiments.sh')],env=env,capture_output=True,text=True)
    assert result.returncode==1, (result.stdout,result.stderr)
    pdir=tmp/'results/p1'
    statuses=list(csv.DictReader((pdir/'run_status.tsv').open(),delimiter='\t'))
    assert len(statuses)==4
    assert any(r['algorithm']=='baseline' and r['exit_code']=='9' for r in statuses)
    assert (pdir/'sparse_natural/repeat_1/SUCCESS').exists()
    runs=list(csv.DictReader((pdir/'analysis/runs.csv').open()))
    assert len(runs)==1 and runs[0]['repeat']=='1' and runs[0]['algorithm']=='sparse'
    assert 'Failed run (exit 9)' in (pdir/'analysis/issues.txt').read_text()
    # Resume reruns the failure but retains successful results without duplicates.
    second=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=env,capture_output=True,text=True)
    assert second.returncode==1
    statuses=list(csv.DictReader((pdir/'run_status.tsv').open(),delimiter='\t'))
    assert len(statuses)==5
    # Changed configuration must not silently reuse prior timings.
    env['COST_WEIGHTS']='2,1,1,1'
    third=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=env,capture_output=True,text=True)
    assert third.returncode==2 and 'another configuration' in third.stderr
print('PASS: failure continuation, warmup filtering, failure reporting, resume, configuration protection')
