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
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'strong_scaling'))
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
     'metadata':{'algorithm':algorithm,'timing_mode':'natural','core_only':'true','description':'sample '*1000,
                 'partition_seed':value('--partition-seed')},
     'metrics':{'core_seconds':1,'local_volume_elements_before_adjacency':10},
     'stages':{'face_pipeline_total':{'seconds':0.1,'calls':1}}}
out=pathlib.Path(value('--profile-dir'))
(out/'rank_profiles.jsonl').write_text(json.dumps(row)+'\\n')
''')
    binary.chmod(0o755)
    # Capability markers used by the runner to reject stale executables.
    with binary.open('a') as f:
        f.write("\n# --algorithm research_1 global_id_bits mesh_phase_v3\n")
    source = tmp/'input.step'
    source.write_text('mock')
    env = dict(os.environ, LOAD_MODULES='0', CLUSTER_ENV_STRICT='0', PROCESS_COUNT='1',
        RUN_ROOT=str(tmp/'results'), BINARY=str(binary), INPUT_PATH=str(source),
        ALGORITHMS='baseline sparse', TIMING_MODES='natural', REPEATS='1', WARMUPS='1',
        MPI_LAUNCHER=str(launcher), MPI_EXTRA_ARGS=' ', START_EPOCH='0', VERIFY_FACES='0',
        RANKS_PER_NODE='1', TIMEOUT_SECONDS='10', CLEANUP_RESULTS='1',
        MESH_EXPERIMENT_WORKER='1',PARTITION_SEEDS='-1',EXPERIMENT_STAGE='legacy',BALANCE_METHOD='boundary')
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
    stages=list(csv.DictReader((pdir/'analysis/stages.csv').open()))
    assert len(stages)==1
    assert stages[0]['algorithm']=='sparse' and stages[0]['repeat']=='1'
    assert stages[0]['stage']=='face_pipeline_total'
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
    kept_env=dict(env, RUN_ROOT=str(tmp/'kept_results'), CLEANUP_RESULTS='0')
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
    stale_env=dict(env, RUN_ROOT=str(tmp/'stale_results'), BINARY=str(stale))
    stale_result=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=stale_env,
                                capture_output=True,text=True)
    assert stale_result.returncode==2 and 'stale/incompatible' in stale_result.stderr
    assert not (tmp/'stale_results/p1').exists()

    # Repeated missing profiles are kept out of the scheduler log.  The user
    # gets one summary and can inspect a dedicated failure file if necessary.
    no_profile=tmp/'no_profile_mesh'
    no_profile.write_text('#!/bin/sh\n# --profile-core-only --algorithm research_1 global_id_bits mesh_phase_v3\nexit 0\n')
    no_profile.chmod(0o755)
    no_profile_env=dict(env, RUN_ROOT=str(tmp/'no_profile_results'), BINARY=str(no_profile),
        ALGORITHMS='baseline sparse', TIMING_MODES='natural', REPEATS='2', WARMUPS='0')
    missing=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=no_profile_env,
                           capture_output=True,text=True)
    assert missing.returncode==1
    assert 'Invalid completed run' not in missing.stderr and missing.stdout.count('RESULT:')==1
    missing_pdir=tmp/'no_profile_results/p1'
    assert len((missing_pdir/'failures.log').read_text().splitlines())==4
    assert '0 / 4' in (missing_pdir/'RESULT_SUMMARY.txt').read_text()

    # Different seed partitions remain different experiment groups, including
    # their warmups, resume identity and expected-count validation.
    multi_env=dict(env,RUN_ROOT=str(tmp/'multi_seed'),ALGORITHMS='sparse',PARTITION_SEEDS='-1 17 41')
    multi=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=multi_env,capture_output=True,text=True)
    assert multi.returncode==0,(multi.stdout,multi.stderr)
    multi_dir=tmp/'multi_seed/p1'
    summaries=list(csv.DictReader((multi_dir/'analysis/summary.csv').open()))
    assert len(summaries)==3 and {int(r['partition_seed']) for r in summaries}=={-1,17,41}
    assert all(r['successful_repeats']=='1' for r in summaries)
    assert '3 / 3' in (multi_dir/'RESULT_SUMMARY.txt').read_text()
    assert len(list(csv.DictReader((multi_dir/'run_status.tsv').open(),delimiter='\t')))==6

    # The public entry submits all configured scales; no parameters are required
    # on its command line. Direct calls to the compatibility entry behave alike.
    submit_base=dict(os.environ, EXPERIMENT_PRESET='pilot', PROCESS_COUNTS='16 32 64',
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
    # Calibration export survives lossless cleanup; old profiles cannot silently
    # become new-model training data. No extra test files or permanent fixtures.
    analysis=runpy.run_path(str(ROOT/'strong_scaling/analyze_results.py'))
    fitting=runpy.run_path(str(ROOT/'strong_scaling/fit_cost_model.py'))
    new_profile=json.loads(profile)
    new_profile['metadata'].update(feature_schema='mesh_phase_v2',cost_model='geometric_proxy',
                                   EXPERIMENT_STAGE='calibration')
    new_profile['metrics'].update({f'phase_feature_{k}':float(k+1) for k in range(8)})
    new_profile['metrics'].update(face_complete_elapsed=0.1,vertex_arrival_elapsed=0.8)
    new_profile['stages']={s:dict(seconds=0.1,calls=1) for s in analysis['COMPUTE']}
    new_run=tmp/'new_profile';new_run.mkdir()
    (new_run/'rank_profiles.jsonl').write_text(json.dumps(new_profile)+'\n')
    assert subprocess.run(analyzer+[str(new_run),'--finish-run'],capture_output=True).returncode==0
    assert subprocess.run(analyzer+[str(new_run)],capture_output=True).returncode==0
    with gzip.open(new_run/'analysis/model_samples.csv.gz','rt') as f:
        exported=list(csv.DictReader(f))
    assert len(exported)==1 and float(exported[0]['y0'])==0.2
    assert float(exported[0]['y3'])==0.2 and exported[0]['repeat']=='1'
    broken=new_profile.copy();broken['metrics']=dict(new_profile['metrics'],vertex_arrival_elapsed=2)
    (new_run/'rank_profiles.jsonl').write_text(json.dumps(broken)+'\n')
    assert subprocess.run(analyzer+[str(new_run),'--finish-run'],capture_output=True).returncode==1
    sample_root=tmp/'calibration'
    for p in (2,3,4):
        folder=sample_root/f'p{p}/analysis';folder.mkdir(parents=True)
        (folder/'issues.txt').write_text('')
        with gzip.open(folder/'model_samples.csv.gz','wt',newline='') as f:
            w=csv.DictWriter(f,fieldnames=analysis['SAMPLE_FIELDS']);w.writeheader()
            for rank in range(p):
                x=[1,rank+1,20/p+rank,25/p+2*rank,rank/3,rank+2,rank+3,p]
                for repeat in (1,2):
                    r={key:'unused' for key in analysis['SAMPLE_META']}
                    r.update(feature_schema='mesh_phase_v2',EXPERIMENT_STAGE='calibration',sampling_protocol='legacy',
                        MESH_INPUT_SHA256='input',MESH_BINARY_SHA256='binary',numlevels='3',numrefine='3',
                        maxh='1000.000000',minh='0.000000',omp_num_threads='1',partition_seed=-1,algorithm='sparse',
                        ranks=p,rank=rank,repeat=repeat,face_complete_elapsed=1,vertex_arrival_elapsed=10)
                    r.update({f'x{k}':value for k,value in enumerate(x)})
                    r.update({f'y{k}':0.1+0.2*(k+1)*x[2] for k in range(4)})
                    w.writerow(r)
    model,meta=fitting['train'](sample_root,8,3,3)
    assert meta['training_ranks']==[2,3,4] and all(v>=0 for v in map(float,model.split()[6:]))
    # Excluded target data may be arbitrary; it must not affect fit/validation.
    poison=sample_root/'p8/analysis';poison.mkdir(parents=True)
    (poison/'model_samples.csv.gz').write_bytes(b'not even a gzip stream')
    again,again_meta=fitting['train'](sample_root,8,3,3)
    assert again==model and again_meta==meta
    frozen=tmp/'p8.model';frozen.write_text(model)
    frozen.with_suffix('.json').write_text(json.dumps(meta))
    fitting['verify'](frozen,'input','binary',3,3,'1000','0','1',8)
    frozen.write_text(model+'0\n')
    try: fitting['verify'](frozen,'input','binary',3,3,'1000','0','1',8)
    except ValueError: pass
    else: raise AssertionError('modified frozen model accepted')
    # The expanded schema cannot be silently populated from old eight-feature
    # runs. Construct distinct, repeated synthetic partition groups in place.
    v3_root=tmp/'calibration_v3'
    for p in (2,3,4):
        folder=v3_root/f'p{p}/analysis';folder.mkdir(parents=True)
        (folder/'issues.txt').write_text('')
        preflight=folder.parent/'partition_preflight';preflight.mkdir()
        report=['seed\tparts\tcoarse_cells\tvariant\tmetis_header\tidx_bits']
        for seed in (-1,17,41):
            labels=[(i//3 if seed==-1 else i if seed==17 else i//2)%p for i in range(3*p)]
            (preflight/f'seed_{seed}.labels').write_text(
                f'mesh_partition_v1 {p} {3*p} {seed} cell_order_v1\n'+'\n'.join(map(str,labels))+'\n')
            report.append(f'{seed}\t{p}\t{3*p}\tcell_order_v1\t5.2.1\t32')
        (preflight/'partitions.tsv').write_text('\n'.join(report)+'\n')
        identity=dict(zip(fitting['IDENTITY'],['input','binary','3','3','1000','0','1']))
        preflight_meta=fitting['seal_preflight'](preflight,p,[-1,17,41],'cell_order_v1',identity,'fixture_source',binary)
        preflight_sha=fitting['digest'](preflight/'manifest.json')
        with gzip.open(folder/'model_samples.csv.gz','wt',newline='') as f:
            w=csv.DictWriter(f,fieldnames=analysis['SAMPLE_FIELDS']);w.writeheader()
            for seed in (-1,17,41):
                for rank in range(p):
                    x=[1,rank+1,20/p+rank,25/p+2*rank,rank/3,rank+2,rank+3,p]+[
                        (rank+1)*(k+1)+seed/100 for k in range(6)]
                    for repeat in (1,2,3):
                        r={key:'unused' for key in analysis['SAMPLE_META']}
                        r.update(feature_schema='mesh_phase_v3',EXPERIMENT_STAGE='calibration',
                            MESH_INPUT_SHA256='input',MESH_BINARY_SHA256='binary',numlevels='3',numrefine='3',
                            maxh='1000',minh='0',omp_num_threads='1',partition_seed=seed,algorithm='sparse',
                            MESH_SOURCE_REVISION='fixture_source',sampling_protocol='partition_sampling_v1',
                            partition_variant='cell_order_v1',rank_shift=(repeat-1)%p,RANKS_PER_NODE='1',
                            MESH_PREFLIGHT_SHA256=preflight_sha,
                            MESH_PARTITION_SIGNATURE=preflight_meta['partitions'][str(seed)]['signature'],
                            ranks=p,rank=(rank+repeat-1)%p,logical_partition=rank,
                            processor_name=f'node{(rank+repeat-1)%p}',repeat=repeat,face_complete_elapsed=1,vertex_arrival_elapsed=10)
                        r.update({f'x{k}':v for k,v in enumerate(x)})
                        r.update({f'y{k}':0.1+0.2*(k+1)*x[2]+0.02*x[8] for k in range(4)})
                        w.writerow(r)
    model3,meta3=fitting['train'](v3_root,8,3,3)
    assert meta3['schema']=='mesh_phase_v3' and meta3['rank_samples']==27
    assert meta3['partition_seeds']==[-1,17,41] and len(meta3['validation'])==6
    assert {r['group_kind'] for r in meta3['validation']}=={'ranks','seed'}
    assert len(meta3['feature_ranges'])==14 and meta3['tail_weight']==4
    assert meta3['identity']['sampling_protocol']=='partition_sampling_v1'
    assert fitting['sampling_readiness'](v3_root)['ready']
    # Correct ordering at small P must not hide reversed ordering at large P.
    ranking=[dict(ranks=p,seed=-1,rank=i,x=[x],y=[y,0,0,0])
             for p,items in [(2,[(10,10),(9,9)]),(4,[(1,2),(2,1)])]
             for i,(x,y) in enumerate(items)]
    scored=fitting['score']([[1],[0],[0],[0]],ranking)
    assert scored['slowest_one_percent_recall']==0.5
    assert scored['worst_group_slowest_relative_error']==-0.5
    poisoned=v3_root/'p8/analysis';poisoned.mkdir(parents=True)
    (poisoned/'model_samples.csv.gz').write_bytes(b'invalid target excluded before read')
    assert fitting['train'](v3_root,8,3,3)==(model3,meta3)
    # A changed seed is not an extra timing repeat; incomplete seed/rank groups
    # invalidate training even if all process counts are present.
    incomplete=v3_root/'p2/analysis/model_samples.csv.gz'
    original=incomplete.read_bytes()
    with gzip.open(incomplete,'rt') as f: samples=list(csv.DictReader(f))
    with gzip.open(incomplete,'wt',newline='') as f:
        w=csv.DictWriter(f,fieldnames=analysis['SAMPLE_FIELDS']);w.writeheader()
        w.writerows(r for r in samples if not (r['partition_seed']=='17' and r['logical_partition']=='1'))
    try: fitting['train'](v3_root,8,3,3)
    except ValueError as e: assert 'missing calibration' in str(e)
    else: raise AssertionError('incomplete partition accepted')
    incomplete.write_bytes(original)
    # 几何样本按逻辑分区归组；节点没有实际改变时不能宣称完成节点对照。
    with gzip.open(incomplete,'wt',newline='') as f:
        w=csv.DictWriter(f,fieldnames=analysis['SAMPLE_FIELDS']);w.writeheader()
        for r in samples:
            r['processor_name']='same_node';w.writerow(r)
    assert not fitting['sampling_readiness'](v3_root/f'p2')['ready']
    incomplete.write_bytes(original)
    reference=v3_root/'p2/partition_preflight/seed_17.labels';saved=reference.read_bytes()
    reference.write_text(reference.read_text().replace('\n1\n','\n0\n',1))
    try: fitting['verify_preflight'](reference.parent)
    except ValueError: pass
    else: raise AssertionError('changed preflight membership accepted')
    reference.write_bytes(saved)
    # New per-rank placement stays outside common run metadata and survives
    # compact export; negative root-only net gain must not be masked by zeros.
    new_profile['metadata'].update(feature_schema='mesh_phase_v3',partition_seed='17')
    new_profile['processor_name']='cn_test'
    new_profile['metrics'].update({f'phase_feature_{k}':float(k+1) for k in range(14)})
    new_profile['metrics']['net_gain_lower_seconds']=-3
    (new_run/'rank_profiles.jsonl').write_text(json.dumps(new_profile)+'\n')
    assert subprocess.run(analyzer+[str(new_run),'--finish-run'],capture_output=True).returncode==0
    assert subprocess.run(analyzer+[str(new_run)],capture_output=True).returncode==0
    with gzip.open(new_run/'analysis/model_samples.csv.gz','rt') as f: sample=next(csv.DictReader(f))
    assert sample['processor_name']=='cn_test' and sample['partition_seed']=='17' and sample['x13']=='14.0'
    assert float(next(csv.DictReader((new_run/'analysis/runs.csv').open()))['net_gain_lower_seconds'])==-3
    # The ordinary submission entry trains all models before any job is sent.
    evaluation=dict(submit_base,EXPERIMENT_STAGE='evaluation',BALANCE_METHOD='boundary',CALIBRATION_ROOT=str(v3_root),
                    PROCESS_COUNTS='8',LEVELS='3',REFINES='3',RUN_ROOT=str(tmp/'evaluation'))
    scheduled=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=evaluation,
                             capture_output=True,text=True)
    assert scheduled.returncode==0,scheduled.stderr
    assert (tmp/'evaluation/models/p8.model').read_text()==model3
    assert (tmp/'evaluation/models/MODEL_REPORT.md').exists()
    blocked=dict(evaluation,CALIBRATION_ROOT=str(sample_root),RUN_ROOT=str(tmp/'blocked_v2'))
    result=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=blocked,capture_output=True,text=True)
    assert result.returncode!=0 and 'needs new multi-seed calibration' in result.stderr
    assert 'yhbatch' not in result.stdout
    # 第三次迭代的完整工作进程路径：单进程预检先行，三次正式映射，失败不生成细网格。
    modern=tmp/'modern_mesh'
    modern.write_text('''#!/usr/bin/env python3
import json,os,pathlib,sys
args=sys.argv
value=lambda name:args[args.index(name)+1]
audit=pathlib.Path(os.environ['MOCK_AUDIT'])
if '--preflight-parts' in args:
    p=int(value('--preflight-parts'));folder=pathlib.Path(value('--preflight-dir'))
    seeds=list(map(int,value('--preflight-seeds').split()))
    report=['seed\\tparts\\tcoarse_cells\\tvariant\\tmetis_header\\tidx_bits']
    for seed in seeds:
        selected=-1 if os.environ.get('MOCK_DUPLICATE_PREFLIGHT') else seed
        labels=[(i//3 if selected==-1 else i if selected==17 else i//2)%p for i in range(3*p)]
        (folder/f'seed_{seed}.labels').write_text(f'mesh_partition_v1 {p} {3*p} {seed} cell_order_v1\\n'+'\\n'.join(map(str,labels))+'\\n')
        report.append(f'{seed}\\t{p}\\t{3*p}\\tcell_order_v1\\t5.2.1\\t32')
    (folder/'partitions.tsv').write_text('\\n'.join(report)+'\\n')
    with audit.open('a') as f:f.write('preflight\\n')
    sys.exit(0)
if '--profile-core-only' not in args:sys.exit(13)
p=int(os.environ['PROCESS_COUNT']);seed=int(value('--partition-seed'))
repeat=int(value('--profile-repeat'));shift=int(value('--rank-shift'))
with audit.open('a') as f:f.write(f'fine {seed} {repeat} {shift}\\n')
metadata={k:os.environ[k] for k in ['MESH_INPUT_SHA256','MESH_BINARY_SHA256','MESH_SOURCE_REVISION',
    'MESH_MODEL_SHA256','MESH_PARTITION_SIGNATURE','MESH_PREFLIGHT_SHA256','EXPERIMENT_STAGE','RANKS_PER_NODE']}
metadata.update(feature_schema='mesh_phase_v3',sampling_protocol='partition_sampling_v1',
    partition_variant='cell_order_v1',rank_shift=str(shift),partition_seed=str(seed),
    algorithm=value('--algorithm'),timing_mode='natural' if '--profile-natural' in args else 'split',
    core_only='true',cost_model='geometric_proxy',
    numlevels=value('-l'),numrefine=value('-r'),maxh=value('--maxh'),minh=value('--minh'),omp_num_threads='1')
metadata['balance_method']='node_mapping' if '--resource-model' in args else 'boundary'
metadata['MESH_CAPACITY_SHA256']=os.environ.get('MESH_CAPACITY_SHA256','none')
rows=[]
for logical in range(p):
    rank=(logical+shift)%p
    if '--rank-capacities' in args and value('--algorithm') in ('balance','combined'):rank=p-1-rank
    metrics=dict(core_seconds=1,local_volume_elements_before_adjacency=100+logical,
        logical_partition=logical,face_complete_elapsed=.1,vertex_arrival_elapsed=.8)
    metrics.update({f'phase_feature_{k}':1 if k==0 else (logical+1)*(k+1)+(seed+1)/100 for k in range(14)})
    stages={s:dict(seconds=.1,calls=1) for s in ['part_face_create','surface_refine','local_volume_mesh',
        'volume_refine','adjacency_build','vertex_numbering_local']}
    rows.append(dict(rank=rank,ranks=p,repeat=repeat,metadata=metadata,metrics=metrics,
                     stages=stages,processor_name=f'node{rank}'))
(pathlib.Path(value('--profile-dir'))/'rank_profiles.jsonl').write_text(''.join(json.dumps(r)+'\\n' for r in rows))
# --algorithm research_1 global_id_bits mesh_resource_v1 --rank-capacities
''')
    # 同一临时程序也模拟任务模式；断言不会调用历史预检、拟合或参考预热。
    task_mock=modern.read_text().replace("metadata={k:os.environ[k]", "metadata={k:os.environ.get(k,'none')")
    task_mock=task_mock.replace("rows=[]", """if '--mesh-tasks' in args:
    metadata.update(feature_schema='mesh_tasks_v1',sampling_protocol='fixed_tasks',balance_method='task_queue',
                    mesh_tasks=value('--mesh-tasks'),active_workers=str(p-1),task_mesh_signature='abcd')
rows=[]""")
    task_mock=task_mock.replace("    rows.append(dict(rank=rank", """    if '--mesh-tasks' in args:
        total=int(value('--mesh-tasks'));completed=0 if rank==0 else total//(p-1)+(rank<=total%(p-1))
        metrics.update(tasks_completed=completed,task_generated_elements_global=total,
                       task_cross_node_faces_before=10,task_cross_node_faces_after=10,
                       task_cross_node_faces_limit=11,task_moved_between_nodes=0,
                       local_volume_elements_before_adjacency=completed*8**int(value('-r')))
        for stage in ['part_face_create','surface_refine','local_volume_mesh']:
            stages[stage]=dict(seconds=.1*completed,calls=completed)
    rows.append(dict(rank=rank""")
    modern.write_text(task_mock+"\n# mesh_tasks_v1 --mesh-tasks\n")
    modern.chmod(0o755)
    calibration=dict(env,EXPERIMENT_STAGE='calibration',PROCESS_COUNT='3',RANKS_PER_NODE='1',
        REPEATS='3',WARMUPS='1',ALGORITHMS='sparse',PARTITION_SEEDS='-1 17 41',
        RUN_ROOT=str(tmp/'modern_results'),BINARY=str(modern),MOCK_AUDIT=str(tmp/'modern_audit'))
    modern_run=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=calibration,
                              capture_output=True,text=True)
    assert modern_run.returncode==0,(modern_run.stdout,modern_run.stderr)
    audit=(tmp/'modern_audit').read_text().splitlines()
    assert audit[0]=='preflight' and len(audit)==13
    assert {int(line.split()[-1]) for line in audit[1:]}=={0,1,2}
    modern_p=tmp/'modern_results/p3'
    assert json.loads((modern_p/'analysis/calibration_status.json').read_text())['ready']
    assert '通过采样检查' in (modern_p/'RESULT_SUMMARY.txt').read_text()
    rerun=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=calibration,
                         capture_output=True,text=True)
    assert rerun.returncode==0,(rerun.stdout,rerun.stderr)
    assert (tmp/'modern_audit').read_text().splitlines()==audit
    bad=dict(calibration,MOCK_DUPLICATE_PREFLIGHT='1',RUN_ROOT=str(tmp/'duplicate_results'),
             MOCK_AUDIT=str(tmp/'duplicate_audit'))
    stopped=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=bad,
                           capture_output=True,text=True)
    assert stopped.returncode==2,(stopped.stdout,stopped.stderr)
    assert (tmp/'duplicate_audit').read_text()=='preflight\n'
    assert '未启动细网格采样' in (tmp/'duplicate_results/p3/RESULT_SUMMARY.txt').read_text()
    # 第四次完整入口：复用刚产生的训练样本，能力预热只一次，四组共用并可续跑。
    import resource_model as resource
    resource_root=tmp/'resource_evaluation'
    resource.train(tmp/'modern_results',resource_root/'models',3,3,3,1)
    resource_env=dict(calibration,EXPERIMENT_STAGE='evaluation',BALANCE_METHOD='node_mapping',
        PARTITION_SEEDS='41',ALGORITHMS='baseline balance sparse combined',TIMING_MODES='natural split',
        REPEATS='1',RUN_ROOT=str(resource_root),MOCK_AUDIT=str(tmp/'resource_audit'))
    mapped=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=resource_env,
                          capture_output=True,text=True)
    assert mapped.returncode==0,(mapped.stdout,mapped.stderr)
    node_report=json.loads((resource_root/'p3/node_capacities.json').read_text())
    assert node_report['slowdown_ratio']>0
    resource_audit=(tmp/'resource_audit').read_text().splitlines()
    assert len(resource_audit)==18 and resource_audit[1]=='fine -1 0 0'
    with (resource_root/'p3/analysis/runs.csv').open() as stream: mapped_runs=list(csv.DictReader(stream))
    assert len(mapped_runs)==8 and {r['algorithm'] for r in mapped_runs}=={'baseline','balance','sparse','combined'}
    assert (resource_root/'p3/capacity_warmup/rank_profiles.jsonl.gz').exists()
    resumed=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=resource_env,
                           capture_output=True,text=True)
    assert resumed.returncode==0,(resumed.stdout,resumed.stderr)
    assert (tmp/'resource_audit').read_text().splitlines()==resource_audit
    # 留出分区的特征/时间不能影响模型训练。
    model_a=(resource_root/'models/p3.mapping').read_bytes()
    samples_path=modern_p/'analysis/model_samples.csv.gz'
    with gzip.open(samples_path,'rt') as stream: source_rows=list(csv.DictReader(stream))
    for row in source_rows:
        if row['partition_seed']=='41':row['x1']='not a number';row['y1']='not a number'
    with gzip.open(samples_path,'wt',newline='') as stream:
        writer=csv.DictWriter(stream,fieldnames=list(source_rows[0]));writer.writeheader();writer.writerows(source_rows)
    resource.train(tmp/'modern_results',tmp/'holdout_poison',3,3,3,1)
    assert (tmp/'holdout_poison/p3.mapping').read_bytes()==model_a
    task_env=dict(env,EXPERIMENT_STAGE='evaluation',BALANCE_METHOD='task_queue',PROCESS_COUNT='3',
        RANKS_PER_NODE='1',TASK_COUNT='8',TASK_CUT_GROWTH='0.10',REPEATS='1',WARMUPS='1',
        PARTITION_SEEDS='41',ALGORITHMS='baseline balance sparse combined',TIMING_MODES='natural split',
        RUN_ROOT=str(tmp/'task_results'),BINARY=str(modern),MOCK_AUDIT=str(tmp/'task_audit'))
    task_run=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=task_env,capture_output=True,text=True)
    assert task_run.returncode==0,(task_run.stdout,task_run.stderr)
    task_p=tmp/'task_results/p3'
    assert not (task_p/'partition_preflight').exists() and not (task_p/'capacity_warmup').exists()
    assert not (task_p/'analysis/model_samples.csv.gz').exists()
    assert not (task_p/'analysis/issues.txt').read_text()
    assert len((tmp/'task_audit').read_text().splitlines())==16
    assert all(line.startswith('fine 41 ') and line.endswith(' 0') for line in (tmp/'task_audit').read_text().splitlines())
    task_resume=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=task_env,capture_output=True,text=True)
    assert task_resume.returncode==0 and len((tmp/'task_audit').read_text().splitlines())==16
    task_submit=dict(submit_base,EXPERIMENT_STAGE='evaluation',BALANCE_METHOD='task_queue',TASK_COUNT='128',
                     PROCESS_COUNTS='16 32 64',CALIBRATION_ROOT='/does/not/exist',RUN_ROOT=str(tmp/'task_submit'))
    submitted=subprocess.run(['bash',str(ROOT/'strong_scaling/run_experiments.sh')],env=task_submit,capture_output=True,text=True)
    assert submitted.returncode==0 and submitted.stdout.count('yhbatch')==3,submitted.stderr
    assert not (tmp/'task_submit/models').exists()
    profile_file=next(task_p.glob('combined*natural/repeat_1/rank_profiles.jsonl.gz'))
    with gzip.open(profile_file,'rt') as f:bad_rows=[json.loads(line) for line in f]
    for row in bad_rows:row['metadata']['task_mesh_signature']='bad'
    with gzip.open(profile_file,'wt') as f:f.write(''.join(json.dumps(row)+'\n' for row in bad_rows))
    analyzed=subprocess.run(analyzer+[str(task_p)],capture_output=True,text=True)
    assert '任务网格不一致' in (task_p/'analysis/issues.txt').read_text(),analyzed.stderr
    assert '不完整，不可直接比较' in (task_p/'RESULT_SUMMARY.txt').read_text()
print('PASS: Slurm spool path, failure continuation, warmup filtering, compressed resume, lossless analysis, cleanup opt-out, failure/active/symlink protection, configuration guard')
