#!/usr/bin/env python3
"""Check the runner, result retention and cleanup using temporary mock outputs."""
import csv
import fcntl
import gzip
import json
import os
import runpy
import shutil
import subprocess
import tempfile
from pathlib import Path
from unittest.mock import patch
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
if '--profile-core-only' not in args: sys.exit(13)
value=lambda name: args[args.index(name)+1]
algorithm=value('--algorithm')
repeat=int(value('--profile-repeat'))
mesh=pathlib.Path(value('-o'))
for folder, name in [('test_occ','test_occ.vol'),('refinedSurfmesh','refinedSurfmesh0.vol'),
                     ('volfined','volfined0.vol'),('volwithadj','volwithadj0.vol'),
                     ('partitioning.1','part.1.elements')]:
    (mesh/folder).mkdir(parents=True,exist_ok=True)
    (mesh/folder/name).write_text('mesh data'*1000)
(mesh/'source.STEP').write_text('keep input')
(mesh/'volfined/notes.txt').write_text('keep notes')
(mesh/'meshQuality').mkdir(exist_ok=True)
(mesh/'meshQuality/meshQuality0.txt').write_text('keep quality')
print('diagnostic log '*1000)
if algorithm=='baseline' and repeat==1: sys.exit(9)
row={'rank':0,'ranks':1,'repeat':repeat,'experiment':algorithm,
     'metadata':{'algorithm':algorithm,'timing_mode':'natural','core_only':'true','description':'sample '*1000},
     'metrics':{'core_seconds':1,'local_volume_elements_before_adjacency':10},
     'stages':{'face_pipeline_total':{'seconds':0.1,'calls':1}}}
out=pathlib.Path(value('--profile-dir'))
(out/'rank_profiles.jsonl').write_text(json.dumps(row)+'\\n')
''')
    binary.chmod(0o755)
    # Capability markers used by the runner to reject stale executables.
    with binary.open('a') as f:
        f.write("\n# --algorithm research_1 global_id_bits\n")
    source = tmp/'input.step'
    source.write_text('mock')
    env = os.environ | dict(LOAD_MODULES='0', CLUSTER_ENV_STRICT='0', PROCESS_COUNT='1',
        RUN_ROOT=str(tmp/'results'), BINARY=str(binary), INPUT_PATH=str(source),
        ALGORITHMS='baseline sparse', TIMING_MODES='natural', REPEATS='1', WARMUPS='1',
        MPI_LAUNCHER=str(launcher), MPI_EXTRA_ARGS=' ', START_EPOCH='0', VERIFY_FACES='0',
        RANKS_PER_NODE='1', TIMEOUT_SECONDS='10', CLEANUP_RESULTS='1',
        MESH_EXPERIMENT_WORKER='1')
    # Reproduce yhbatch: execute a copy named slurm_script outside the source
    # tree while STRONG_SCALING_DIR points back to the real companion scripts.
    spooled=tmp/'slurm_script'
    shutil.copy2(ROOT/'strong_scaling/run_experiments.sh',spooled)
    env['STRONG_SCALING_DIR']=str(ROOT/'strong_scaling')
    result = subprocess.run(['bash', str(spooled)],env=env,capture_output=True,text=True)
    assert result.returncode==1, (result.stdout,result.stderr)
    pdir=tmp/'results/p1'
    statuses=list(csv.DictReader((pdir/'run_status.tsv').open(),delimiter='\t'))
    assert len(statuses)==4
    assert any(r['algorithm']=='baseline' and r['exit_code']=='9' for r in statuses)
    assert (pdir/'sparse_natural/repeat_1/SUCCESS').exists()
    success=pdir/'sparse_natural/repeat_1'
    failed=pdir/'baseline_natural/repeat_1'
    assert (success/'rank_profiles.jsonl.gz').exists() and not (success/'rank_profiles.jsonl').exists()
    assert (success/'run.log.gz').exists() and 'diagnostic log' in gzip.open(success/'run.log.gz','rt').read()
    assert not (success/'mesh/volfined/volfined0.vol').exists()
    assert not (success/'mesh/partitioning.1/part.1.elements').exists()
    assert (success/'mesh/source.STEP').read_text()=='keep input'
    assert (success/'mesh/volfined/notes.txt').exists() and (success/'mesh/meshQuality/meshQuality0.txt').exists()
    assert (failed/'mesh/volfined/volfined0.vol').exists() and (failed/'run.log').exists()
    runs=list(csv.DictReader((pdir/'analysis/runs.csv').open()))
    assert len(runs)==1 and runs[0]['repeat']=='1' and runs[0]['algorithm']=='sparse'
    assert 'Failed run (exit 9)' in (pdir/'analysis/issues.txt').read_text()
    overview=(pdir/'RESULT_SUMMARY.txt').read_text()
    assert '不完整，不可直接比较' in overview and '1 / 2' in overview
    assert result.stderr.count('Invalid completed run')==0
    # Resume reruns the failure but retains successful results without duplicates.
    second=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=env,capture_output=True,text=True)
    assert second.returncode==1
    statuses=list(csv.DictReader((pdir/'run_status.tsv').open(),delimiter='\t'))
    assert len(statuses)==5
    analyzer=['python3',str(ROOT/'strong_scaling/analyze_results.py')]
    csv_before=(pdir/'analysis/summary.csv').read_bytes()
    repeat_analysis=subprocess.run(analyzer+[str(pdir)],capture_output=True,text=True)
    assert repeat_analysis.returncode==0 and (pdir/'analysis/summary.csv').read_bytes()==csv_before

    # Opt-out retains all artifacts; later analysis can reclaim them identically.
    kept_env=env | dict(RUN_ROOT=str(tmp/'kept_results'), CLEANUP_RESULTS='0')
    kept=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=kept_env,capture_output=True,text=True)
    assert kept.returncode==1, kept.stderr
    kept_pdir=tmp/'kept_results/p1'
    kept_run=kept_pdir/'sparse_natural/repeat_1'
    assert (kept_run/'rank_profiles.jsonl').exists() and (kept_run/'mesh/volfined/volfined0.vol').exists()
    saved_summary=(kept_pdir/'analysis/summary.csv').read_bytes()
    assert subprocess.run(analyzer+[str(kept_pdir)],capture_output=True).returncode==0
    assert (kept_pdir/'analysis/summary.csv').read_bytes()==saved_summary
    assert not (kept_run/'mesh/volfined/volfined0.vol').exists()

    # No cleanup of unverified runs, active jobs, external symlinks or verification outputs.
    profile=gzip.open(success/'rank_profiles.jsonl.gz','rt').read()
    invalid=tmp/'invalid_run';invalid.mkdir()
    (invalid/'rank_profiles.jsonl').write_text('{broken json')
    (invalid/'mesh/volfined').mkdir(parents=True)
    (invalid/'mesh/volfined/volfined0.vol').write_text('preserve')
    assert subprocess.run(analyzer+[str(invalid),'--finish-run'],capture_output=True).returncode==1
    assert not (invalid/'SUCCESS').exists() and (invalid/'mesh/volfined/volfined0.vol').exists()
    full_io=json.loads(profile);full_io['metadata']['core_only']='false'
    (invalid/'rank_profiles.jsonl').write_text(json.dumps(full_io))
    assert subprocess.run(analyzer+[str(invalid),'--finish-run'],capture_output=True).returncode==1
    assert (invalid/'mesh/volfined/volfined0.vol').exists()
    compressor=runpy.run_path(str(ROOT/'strong_scaling/analyze_results.py'))['compress_result']
    original=tmp/'compression_failure.log';original.write_text('preserve all bytes'*1000)
    with patch('shutil.copyfileobj',side_effect=OSError('simulated disk full')):
        try:
            compressor(original)
            raise AssertionError('compression should fail')
        except OSError:
            pass
    assert original.read_text()=='preserve all bytes'*1000 and not original.with_suffix('.log.gz').exists()
    active=success/'mesh/volfined/volfined7.vol';active.write_text('active data')
    with (success/'.run.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        assert subprocess.run(analyzer+[str(success),'--finish-run'],capture_output=True).returncode==1
        assert active.exists()
    (success/'RUNNING').touch()
    assert subprocess.run(analyzer+[str(success),'--finish-run'],capture_output=True).returncode==1
    assert active.exists()
    (success/'RUNNING').unlink()
    outside=tmp/'external';outside.mkdir()
    outside_file=outside/'volwithadj0.vol';outside_file.write_text('external data')
    (success/'mesh/volwithadj').symlink_to(outside,target_is_directory=True)
    (success/'mesh/volfined/volfined99.vol').symlink_to(outside_file)
    verify=pdir/'verify_sparse';verify.mkdir()
    (verify/'volwithadj0.vol').write_text('retain for comparison')
    assert subprocess.run(analyzer+[str(pdir)],capture_output=True).returncode==0
    assert not active.exists() and outside_file.read_text()=='external data'
    assert (success/'mesh/volfined/volfined99.vol').is_symlink() and (verify/'volwithadj0.vol').exists()
    linked=tmp/'linked_run';linked.mkdir()
    (linked/'rank_profiles.jsonl').write_text(profile)
    (linked/'mesh').symlink_to(outside,target_is_directory=True)
    assert subprocess.run(analyzer+[str(linked),'--finish-run'],capture_output=True).returncode==0
    assert outside_file.read_text()=='external data'
    # Changed configuration must not silently reuse prior timings.
    env['COST_WEIGHTS']='2,1,1,1'
    third=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=env,capture_output=True,text=True)
    assert third.returncode==2 and 'another configuration' in third.stderr

    # A stale executable is rejected before any expensive mesh run starts.
    stale=tmp/'stale_mesh';stale.write_text('#!/bin/sh\nexit 0\n');stale.chmod(0o755)
    stale_env=env | dict(RUN_ROOT=str(tmp/'stale_results'), BINARY=str(stale))
    stale_result=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=stale_env,
                                capture_output=True,text=True)
    assert stale_result.returncode==2 and 'stale/incompatible' in stale_result.stderr
    assert not (tmp/'stale_results/p1').exists()

    # Repeated missing profiles are kept out of the scheduler log.  The user
    # gets one summary and can inspect a dedicated failure file if necessary.
    no_profile=tmp/'no_profile_mesh'
    no_profile.write_text('#!/bin/sh\n# --profile-core-only --algorithm research_1 global_id_bits\nexit 0\n')
    no_profile.chmod(0o755)
    no_profile_env=env | dict(RUN_ROOT=str(tmp/'no_profile_results'), BINARY=str(no_profile),
        ALGORITHMS='baseline sparse', TIMING_MODES='natural', REPEATS='2', WARMUPS='0')
    missing=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=no_profile_env,
                           capture_output=True,text=True)
    assert missing.returncode==1
    assert 'Invalid completed run' not in missing.stderr and missing.stdout.count('RESULT:')==1
    missing_pdir=tmp/'no_profile_results/p1'
    assert len((missing_pdir/'failures.log').read_text().splitlines())==4
    assert '0 / 4' in (missing_pdir/'RESULT_SUMMARY.txt').read_text()

    # The public entry submits all configured scales; no parameters are required
    # on its command line. Direct calls to the compatibility entry behave alike.
    submit_base=os.environ | dict(EXPERIMENT_PRESET='pilot', PROCESS_COUNTS='16 32 64',
        RUN_ROOT=str(tmp/'submitted'), DRY_RUN='1', RANKS_PER_NODE='16',
        START_EPOCH='1', SBATCH_COMMAND='yhbatch')
    for entry in ('run_experiments.sh','submit_experiments.sh'):
        submitted=subprocess.run(['bash',str(ROOT/'strong_scaling'/entry)],env=submit_base,
                                 capture_output=True,text=True)
        assert submitted.returncode==0, (entry,submitted.stdout,submitted.stderr)
        commands=[line for line in submitted.stdout.splitlines() if 'yhbatch' in line]
        assert len(commands)==3 and all('run_experiments.sh' in line for line in commands)
        assert any('-N 1 ' in line and '-n 16 ' in line for line in commands)
        assert any('-N 2 ' in line and '-n 32 ' in line for line in commands)
        assert any('-N 4 ' in line and '-n 64 ' in line for line in commands)
print('PASS: Slurm spool path, failure continuation, warmup filtering, compressed resume, lossless analysis, cleanup opt-out, failure/active/symlink protection, configuration guard')
