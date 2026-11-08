#!/usr/bin/python3
"""Short wrapper smoke with fake CLI/ifconfig output only."""
import importlib.util,json,struct,unittest,sys
sys.dont_write_bytecode=True
from pathlib import Path
spec=importlib.util.spec_from_file_location('service',Path(__file__).with_name('cx3-network-service.py'))
s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
class Smoke(unittest.TestCase):
 def setUp(self):
  self.config={'schema':'cx3-network-service-v1','enabled':True,'dhcp':True,'cli':'/fake/cx3ctl','expectedBuild':'0.44.131-local'}
  scalars=[0,0,2,1,1,0xec0d9a2a1511]+[0]*10
  raw=struct.pack('<IIQ8I3QII16Q',1,208,0,2,1,16,16,0,39,2,1,2,65535,0,3,0,*scalars)
  self.e={'schema':'cx3-auto-network-v1','ok':True,'bound':True,'control':{'transport':0,'returnedBytes':208,'rawHex':raw.hex()}}
 def test_success(self):
  self.assertEqual(s.decode_control(self.e,1),('en2','00:11:22:33:44:55'))
  seen=[]
  def run(cmd):
   seen.append(cmd)
   if cmd[0]=='/fake/cx3ctl':return json.dumps(self.e)
   if cmd==['/sbin/ifconfig','en2']:return 'en2: flags=0\n ether 00:11:22:33:44:55\n'
   return ''
  old=s.run;s.run=run
  try:r=s.control(self.config,'enable',True)
  finally:s.run=old
  self.assertEqual(seen[-2:], [['/sbin/ifconfig','en2','up'],['/usr/sbin/ipconfig','set','en2','DHCP']])
  self.assertFalse(r['leaseVerified'])
 def test_reject_bad_binding_or_mac(self):
  self.e['bound']=False
  with self.assertRaises(s.Stop):s.decode_control(self.e,1)
  self.e['bound']=True;seen=[]
  def run(cmd):seen.append(cmd);return json.dumps(self.e) if len(seen)==1 else 'ether 00:11:22:33:44:55'
  old=s.run;s.run=run
  try:
   with self.assertRaises(s.Stop):s.control(self.config,'enable',True)
  finally:s.run=old
  self.assertEqual(len(seen),2) # no OS mutation after mismatch
 def test_dry_run(self):
  old=s.run;s.run=lambda command:(_ for _ in ()).throw(AssertionError('transport called'))
  try:self.assertTrue(s.control(self.config,'enable',False)['dryRun'])
  finally:s.run=old
if __name__=='__main__':unittest.main()
