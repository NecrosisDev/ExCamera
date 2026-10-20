"""Optional standard-library mocks; does not execute a Windows build or OS lock."""
from pathlib import Path
import importlib.util,io,os,sys,tempfile,unittest
from unittest.mock import patch
spec=importlib.util.spec_from_file_location("excamera_build",Path(__file__).resolve().parents[1]/"build.py")
BUILD=importlib.util.module_from_spec(spec);spec.loader.exec_module(BUILD)
from types import SimpleNamespace
# Mock msvcrt call sequencing only; this does not exercise Windows OS locks.
def fake_lock(fd, mode, count):
 assert count == 1 and os.lseek(fd,0,os.SEEK_CUR)==0
FAKE_CRT=SimpleNamespace(locking=fake_lock,LK_NBLCK=1,LK_UNLCK=2)
REPO=Path(__file__).resolve().parents[1]
class BuildPublicationTests(unittest.TestCase):
 def setUp(self):
  self.crt=patch.dict(sys.modules,{"msvcrt":FAKE_CRT});self.crt.start();self.addCleanup(self.crt.stop)
 def test_lock_file_presence_is_not_a_sentinel_and_handle_is_not_inherited(self):
  with tempfile.TemporaryDirectory() as td:
   out=Path(td);(out/".build.lock").write_text("left from prior crash")
   calls=[]
   def lock(fd,mode,count):
    self.assertFalse(os.get_inheritable(fd));self.assertEqual(os.lseek(fd,0,os.SEEK_CUR),0);calls.append(mode)
   with patch.object(FAKE_CRT,"locking",lock):
    for attempt in range(2):
     with self.assertRaises(RuntimeError),BUILD.build_lock(out):raise RuntimeError("handled failure")
   self.assertEqual(calls,[1,2,1,2]);self.assertTrue((out/".build.lock").is_file())

 def test_all_publication_failures_with_rollback(self):
  for failed_name in ['dll','exe','zip']:
   with self.subTest(failed_name=failed_name),tempfile.TemporaryDirectory() as td:
    out=Path(td);names=['dll','exe','zip'];[ (out/n).write_text('old') for n in names];(out/'unrelated').write_text('keep')
    def build(stage):[(stage/n).write_text('new') for n in names]
    def replace(src,dst):
     if dst.name==failed_name and src.parent.name!='previous':raise PermissionError('locked')
     os.replace(src,dst)
    with self.assertRaisesRegex(RuntimeError,'previous published outputs were retained'):BUILD.staged_publish(out,names,build,replace)
    self.assertEqual([(out/n).read_text() for n in names],['old']*3);self.assertEqual((out/'unrelated').read_text(),'keep')
 def test_unrecoverable_rollback_keeps_recovery_files(self):
  with tempfile.TemporaryDirectory() as td:
   out=Path(td);names=['dll','exe','zip'];[(out/n).write_text('old') for n in names]
   def build(stage):[(stage/n).write_text('new') for n in names]
   def replace(src,dst):
    if dst.name=='exe' or src.parent.name=='previous':raise PermissionError('locked')
    os.replace(src,dst)
   with self.assertRaisesRegex(RuntimeError,'Publication incomplete'):BUILD.staged_publish(out,names,build,replace)
   self.assertEqual(next(out.glob('.stage-*/previous/dll')).read_text(),'old')
 def test_interrupt_after_replace_restores_previous_or_absent_target(self):
  for existing in (False,True):
   with self.subTest(existing=existing),tempfile.TemporaryDirectory() as td:
    out=Path(td)
    if existing:(out/'dll').write_text('old')
    def build(stage):(stage/'dll').write_text('new')
    def replace(src,dst):
     os.replace(src,dst)
     if src.parent.name!='previous':raise KeyboardInterrupt('after replace')
    with self.assertRaisesRegex(RuntimeError,'previous published outputs were retained'):BUILD.staged_publish(out,['dll'],build,replace)
    self.assertEqual((out/'dll').read_text() if existing else (out/'dll').exists(),'old' if existing else False)
 def test_backup_copy_failure_changes_no_targets(self):
  with tempfile.TemporaryDirectory() as td:
   out=Path(td);(out/'dll').write_text('old')
   def build(stage):(stage/'dll').write_text('new')
   with patch.object(BUILD.shutil,'copy2',side_effect=OSError('backup failed')),self.assertRaisesRegex(RuntimeError,'backup failed'):
    BUILD.staged_publish(out,['dll'],build)
   self.assertEqual((out/'dll').read_text(),'old')
 def test_cleanup_warning_preserves_success_or_primary_failure_and_releases_lock(self):
  for failure in (False,True,'rollback'):
   with self.subTest(failure=failure),tempfile.TemporaryDirectory() as td:
    out=Path(td);(out/'dll').write_text('old');calls=[];stderr=io.StringIO()
    def build(stage):
     (stage/'dll').write_text('new')
     if failure is True:raise ValueError('primary compile failure')
    def replace(src,dst):
     if failure=='rollback' and src.parent.name!='previous':raise PermissionError('primary publication failure')
     os.replace(src,dst)
    with patch.object(FAKE_CRT,'locking',lambda fd,mode,count:calls.append(mode)),patch.object(BUILD.shutil,'rmtree',side_effect=PermissionError('cleanup locked')),patch('sys.stderr',stderr):
     if failure:
      with self.assertRaisesRegex(RuntimeError,'primary (compile|publication) failure'):BUILD.staged_publish(out,['dll'],build,replace)
     else:BUILD.staged_publish(out,['dll'],build)
    self.assertEqual(calls,[1,2]);self.assertIn('staging cleanup failed',stderr.getvalue());self.assertEqual((out/'dll').read_text(),'old' if failure else 'new')
 def test_nonfile_and_symlink_targets_refused(self):
  for link in (False,True):
   with self.subTest(link=link),tempfile.TemporaryDirectory() as td:
    out=Path(td)
    if link:
     (out/'custom').write_text('custom')
     try:(out/'dll').symlink_to(out/'custom')
     except OSError:self.skipTest('Host does not permit symlink creation')
    else:(out/'dll').mkdir()
    def build(stage):(stage/'dll').write_text('new')
    with self.assertRaisesRegex(RuntimeError,'Refusing non-file or symlink'):BUILD.staged_publish(out,['dll'],build)
    self.assertTrue((out/'dll').is_symlink() if link else (out/'dll').is_dir())
 def test_batch_generation_and_mock_process_failure(self):
  # Actual generated batch and subprocess forwarding; NOT cmd.exe execution.
  namespace=dict(BUILD.__dict__)
  namespace['run']=BUILD.run
  with tempfile.TemporaryDirectory() as td,patch.dict(os.environ,{'ERRORLEVEL':'0'}):
   patches=patch.multiple(BUILD,SRC=Path('C:/source with spaces'),vcvars=lambda:Path('C:/VS/vcvars64.bat'))
   patches.start();self.addCleanup(patches.stop)
   for code in (0,1,2,-1):
    with patch.object(namespace['subprocess'],'run',return_value=type('Result',(),{'returncode':code})()):
     if code:
      with self.assertRaises(SystemExit):namespace['run'](['cl test.cpp','findstr /r /c:" enable$" "exports.txt" >nul'],Path(td))
     else:namespace['run'](['cl test.cpp','findstr /r /c:" enable$" "exports.txt" >nul'],Path(td))
   text=(Path(td)/'build.cmd').read_text();self.assertNotIn('%errorlevel%',text.lower())
   for line in text.splitlines()[1:-1]:
    if line != 'setlocal DisableDelayedExpansion':self.assertTrue(line.endswith(' || exit /b 1'),line)
   self.assertEqual(text.splitlines()[-1],'exit /b 0')
 def test_actual_main_builds_and_packages_only_staged_outputs(self):
  with tempfile.TemporaryDirectory() as td:
   root=Path(td);src=root/'src';src.mkdir();(src/'vendor/minhook').mkdir(parents=True)
   ver='0.3.0-rc.1';(src/'build_identity.h').write_text('#define FP_BUILD "'+ver+'"\n')
   (root/'LICENSE').write_text('main license');(src/'vendor/minhook/LICENSE.txt').write_text('MinHook license')
   out=root/'build';out.mkdir();(out/'unrelated').write_text('keep');calls=[]
   def run(commands,stage):
    self.assertNotEqual(stage,out);self.assertEqual(stage.parent,out);calls.extend(commands)
    (stage/'ExanimaFirstPerson.dll').write_bytes(b'new DLL');(stage/'ExCamera.exe').write_bytes(b'new EXE')
   with patch.multiple(BUILD,ROOT=root,SRC=src,OUT=out,run=run),patch.object(sys,'argv',['build.py','--package']),patch('sys.stdout',io.StringIO()) as stdout:
    BUILD.main()
   self.assertIn('BUILT '+ver,stdout.getvalue());self.assertIn('PACKAGE',stdout.getvalue())
   self.assertEqual((out/'unrelated').read_text(),'keep')
   import zipfile
   with zipfile.ZipFile(out/('ExCamera-'+ver+'-minimal.zip')) as z:
    self.assertEqual(set(z.namelist()),{'ExanimaFirstPerson.dll','ExCamera.exe','LICENSE.txt','LICENSE-MinHook.txt'})
    self.assertEqual(z.read('ExanimaFirstPerson.dll'),b'new DLL');self.assertIsNone(z.testzip())
   self.assertEqual(BUILD.CL,'cl /nologo /MT /O2 /std:c++17 /EHsc /fp:strict /W4 /Brepro')
   self.assertEqual(BUILD.MINHOOK_CL,'cl /nologo /MT /O2 /W1 /Brepro /c')
   self.assertEqual(len(calls),10);self.assertIn('/c native_settings.cpp settings_presentation.cpp',calls[0])
   link=calls[3]
   for name in ['buffer.obj','hook.obj','trampoline.obj','hde64.obj','supersampling_shim.obj','native_settings.obj','settings_presentation.obj']:self.assertIn(name,link)
   for command in calls:self.assertIn('.stage-',command)
 def test_actual_compile_failure_preserves_old_artifacts_and_has_no_success_message(self):
  with tempfile.TemporaryDirectory() as td:
   out=Path(td);(out/'ExanimaFirstPerson.dll').write_text('old DLL');(out/'ExCamera.exe').write_text('old EXE');(out/'ExCamera-0.2.0-minimal.zip').write_text('old ZIP')
   def fail(commands,stage):(stage/'ExanimaFirstPerson.dll').write_text('partial');raise SystemExit('mock compiler failed')
   with patch.multiple(BUILD,OUT=out,run=fail),patch.object(sys,'argv',['build.py','--package']),patch('sys.stdout',io.StringIO()) as stdout,self.assertRaisesRegex(RuntimeError,'retained, not rebuilt'):
    BUILD.main()
   self.assertNotIn('BUILT',stdout.getvalue());self.assertEqual((out/'ExanimaFirstPerson.dll').read_text(),'old DLL');self.assertEqual((out/'ExCamera-0.2.0-minimal.zip').read_text(),'old ZIP')

 def test_nonpackage_success_explicitly_leaves_existing_zip_unchanged(self):
  with tempfile.TemporaryDirectory() as td:
   out=Path(td);package=out/'ExCamera-0.2.0-minimal.zip';package.write_bytes(b'previous ZIP')
   def run(commands,stage):
    (stage/'ExanimaFirstPerson.dll').write_bytes(b'new DLL');(stage/'ExCamera.exe').write_bytes(b'new EXE')
   with patch.multiple(BUILD,OUT=out,run=run),patch.object(sys,'argv',['build.py']),patch('sys.stdout',io.StringIO()) as stdout:
    BUILD.main()
   self.assertEqual(package.read_bytes(),b'previous ZIP');self.assertIn('No ZIP was refreshed',stdout.getvalue())
 def test_archive_write_failure_does_not_touch_previous_outputs(self):
  with tempfile.TemporaryDirectory() as td:
   out=Path(td);package=out/'ExCamera-0.2.0-minimal.zip';package.write_bytes(b'previous ZIP');(out/'ExCamera.exe').write_bytes(b'old EXE')
   def run(commands,stage):
    (stage/'ExanimaFirstPerson.dll').write_bytes(b'new DLL');(stage/'ExCamera.exe').write_bytes(b'new EXE')
   with patch.multiple(BUILD,OUT=out,run=run),patch.object(sys,'argv',['build.py','--package']),patch.object(BUILD.zipfile.ZipFile,'writestr',side_effect=OSError('archive full')),self.assertRaisesRegex(RuntimeError,'retained, not rebuilt'):
    BUILD.main()
   self.assertEqual(package.read_bytes(),b'previous ZIP');self.assertEqual((out/'ExCamera.exe').read_bytes(),b'old EXE');self.assertFalse((out/'ExanimaFirstPerson.dll').exists())

class BuildPathTransportTests(unittest.TestCase):
 def test_actual_commands_preserved_across_checkout_path_characters(self):
  # Exercise actual build_runtime -> run generation, not a cmd/Windows build.
  for folder in ('René_日本', 'space here', 'A&B', 'A%TEMP%B', 'A!TEMP!B', 'A(B)'):
   with self.subTest(folder=folder),tempfile.TemporaryDirectory() as td:
    root=Path(td)/folder;src=root/'src';stage=root/'build'/'.stage-test'
    src.mkdir(parents=True);stage.mkdir(parents=True)
    toolchain=Path(td)/'Visual Studio_日本'/'vcvars64.bat'
    commands=[]
    original_run=BUILD.run
    def capture_run(current,where):
     commands.extend(current)
     return original_run(current,where)
    with patch.dict(os.environ,{'NoDefaultCurrentDirectoryInExePath':'1',
                                'EXCAMERA_BUILD_SOURCE':'parent value'}):
     before=dict(os.environ)
     with patch.multiple(BUILD,SRC=src,vcvars=lambda:toolchain,run=capture_run),patch.object(
         BUILD.subprocess,'run',return_value=SimpleNamespace(returncode=0)) as process:
      BUILD.build_runtime(stage,'test',False)
     self.assertEqual(dict(os.environ),before)
    args,kwargs=process.call_args
    self.assertEqual(args,(['cmd.exe','/d','/v:off','/c',r'.\build.cmd'],))
    self.assertEqual(kwargs['cwd'],stage)
    self.assertEqual(kwargs['env']['EXCAMERA_BUILD_SOURCE'],str(src))
    self.assertEqual(kwargs['env']['EXCAMERA_BUILD_STAGE'],str(stage))
    self.assertEqual(kwargs['env']['EXCAMERA_BUILD_VCVARS'],str(toolchain))
    self.assertEqual(kwargs['env']['NoDefaultCurrentDirectoryInExePath'],'1')
    self.assertEqual({k:v for k,v in kwargs['env'].items() if not k.startswith('EXCAMERA_BUILD_')},
                     {k:v for k,v in before.items() if not k.startswith('EXCAMERA_BUILD_')})
    script=(stage/'build.cmd').read_bytes().decode('ascii').splitlines()
    self.assertEqual(script[:5],[
        '@echo off','setlocal DisableDelayedExpansion',
        'call "%EXCAMERA_BUILD_VCVARS%" >nul || exit /b 1',
        'setlocal DisableDelayedExpansion',
        'cd /d "%EXCAMERA_BUILD_SOURCE%" || exit /b 1'])
    self.assertEqual(script[-1],'exit /b 0')
    self.assertEqual(len(commands),10)
    prefix='"%EXCAMERA_BUILD_STAGE%'+os.sep
    restored=[line[:-len(' || exit /b 1')].replace(prefix,'"'+str(stage)+os.sep)
              for line in script[5:-1]]
    self.assertEqual(restored,commands)
    for line in script[5:-1]:self.assertTrue(line.endswith(' || exit /b 1'))
    self.assertIn('/Fo:"'+str(stage/'obj')+'\\\\"',restored[0])
    self.assertNotIn('chcp','\n'.join(script).lower())
    self.assertNotIn(str(root),'\n'.join(script))
    self.assertEqual(BUILD.CL,'cl /nologo /MT /O2 /std:c++17 /EHsc /fp:strict /W4 /Brepro')

 def test_only_exact_quoted_stage_prefix_is_transported(self):
  with tempfile.TemporaryDirectory() as td:
   stage=Path(td)
   quoted='"'+str(stage)+os.sep+'obj'+os.sep+'item.obj"'
   unrelated='"'+str(stage)+'-other'+os.sep+'item.obj"'
   command='cl '+quoted+' '+unrelated+' bare='+str(stage)
   with patch.object(BUILD,'vcvars',return_value=Path('C:/VS/vcvars64.bat')),patch.object(
       BUILD.subprocess,'run',return_value=SimpleNamespace(returncode=0)):
    BUILD.run([command],stage)
   line=(stage/'build.cmd').read_text().splitlines()[5]
   self.assertEqual(line,'cl "%EXCAMERA_BUILD_STAGE%'+os.sep+'obj'+os.sep+
                    'item.obj" '+unrelated+' bare='+str(stage)+' || exit /b 1')

 def test_vswhere_requests_and_decodes_utf8_with_optional_bom(self):
  for bom in (b'',b'\xef\xbb\xbf'):
   with self.subTest(bom=bom):
    expected='C:\\Visual Studio_日本\\é'
    def output(args,**kwargs):
     self.assertEqual(args[-3:],['-property','installationPath','-utf8'])
     self.assertTrue(kwargs['text'])
     self.assertEqual(kwargs['encoding'],'utf-8-sig')
     return (bom+(expected+'\r\n').encode('utf-8')).decode(kwargs['encoding'])
    with patch.object(BUILD.subprocess,'check_output',side_effect=output),patch.object(
        Path,'is_file',return_value=True):
     self.assertEqual(BUILD.vcvars(),Path(expected)/'VC/Auxiliary/Build/vcvars64.bat')

 def test_process_start_and_vswhere_errors_propagate(self):
  with tempfile.TemporaryDirectory() as td:
   with patch.object(BUILD,'vcvars',return_value=Path('C:/VS/vcvars64.bat')),patch.object(
       BUILD.subprocess,'run',side_effect=OSError('cannot start cmd')):
    with self.assertRaisesRegex(OSError,'cannot start cmd'):BUILD.run([],Path(td))
   with patch.object(BUILD.subprocess,'check_output',side_effect=OSError('cannot find vswhere')):
    with self.assertRaisesRegex(OSError,'cannot find vswhere'):BUILD.run([],Path(td))

if __name__=='__main__':unittest.main(verbosity=2)
