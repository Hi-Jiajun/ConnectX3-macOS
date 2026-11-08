#!/usr/bin/env python3
"""Exercise saved-log consumer against the production fault logger, offline only."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('tx_fault_decode', ROOT/'decode-tx-fault.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


class FaultDecode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='cx3-tx-log-test-')
        cls.log = Path(cls.temporary.name)/'production.log'
        result = subprocess.run(['/usr/bin/python3',str(ROOT/'test-net-tx-fault.py'),
                                 '--fault-log',str(cls.log)], capture_output=True,text=True,timeout=90)
        if result.returncode: raise AssertionError(result.stdout+result.stderr)
        cls.text = cls.log.read_text()

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_production_logger_bytes_and_limitations(self):
        d = decoder.decode(self.text,256)
        self.assertEqual((d['syndrome_name'],d['vendor_syndrome']),('LOCAL_QP_OP_ERR',0x6d))
        self.assertEqual((d['wqe_index'],d['slot'],d['inferred_post_counter']),(600,88,197208))
        self.assertEqual((d['wqe']['opcode'],d['wqe']['byte_count'],d['wqe']['lkey']),(10,64,65536))
        self.assertEqual(d['wqe']['dma_address'],'0x0000000123456000')
        self.assertFalse(d['hardware_root_cause_proven'])
        self.assertFalse(d['runtime_identity_verified'])
        self.assertFalse(d['counter_inference_is_exact_record'])
        self.assertEqual(d['issues'],[])

    def test_truncated_each_row(self):
        for line in self.text.splitlines()[1:]:
            with self.subTest(line=line),self.assertRaises(ValueError):
                decoder.decode(self.text.replace(line+'\n',''),256)

    def test_duplicate_summary(self):
        with self.assertRaises(ValueError): decoder.decode(self.text+self.text,256)

    def test_duplicate_raw_row(self):
        with self.assertRaises(ValueError): decoder.decode(self.text+self.text.splitlines()[-1]+'\n',256)

    def test_malformed_raw_word(self):
        with self.assertRaises(ValueError): decoder.decode(self.text.replace('0002580a','000258zz'),256)

    def test_inconsistent_summary(self):
        for old,new in [('syndrome 0x02','syndrome 0x05'),('vendor 0x6d','vendor 0xf9'),
                        ('wqe_index 600','wqe_index 601'),('cqe_qpn 0x29','cqe_qpn 0x28')]:
            with self.subTest(old=old),self.assertRaises(ValueError):
                decoder.decode(self.text.replace(old,new),256)

    def test_unknown_vendor_not_invented(self):
        d=decoder.decode(self.text,256)
        self.assertEqual(d['vendor_meaning'],'unknown')

    def test_missing_wqe_is_explicit(self):
        text='\n'.join(l for l in self.text.replace('WQE_valid 1','WQE_valid 0').splitlines()
                       if 'FIRST WQE' not in l)
        d=decoder.decode(text,256)
        self.assertTrue(d['complete']);self.assertIsNone(d['wqe'])
        self.assertIn('WQE snapshot unavailable',d['issues'])

    def test_wqe_invalid_flag_with_rows_rejected(self):
        with self.assertRaises(ValueError):decoder.decode(self.text.replace('WQE_valid 1','WQE_valid 0'),256)

    def test_descriptor_anomaly_preserved(self):
        d=decoder.decode(self.text.replace('0002580a','80025800'),256)
        self.assertIn('WQE owner differs from posted owner record',d['issues'])
        self.assertIn('WQE opcode is not SEND',d['issues'])
        self.assertEqual(d['wqe']['raw_hex'][:8],'80025800')

    def test_inference_requires_outstanding_window(self):
        d=decoder.decode(self.text.replace('producer 197209','producer 197208'),256)
        self.assertIsNone(d['inferred_post_counter'])
        d=decoder.decode(self.text.replace('mbuf 1','mbuf 0'),256)
        self.assertIsNone(d['inferred_post_counter'])

    def test_sq_entries_not_guessed(self):
        for entries in (0,255,65537,True):
            with self.subTest(entries=entries),self.assertRaises(ValueError):decoder.decode(self.text,entries)

    def test_cli_success_and_failure(self):
        cmd=['/usr/bin/python3',str(ROOT/'decode-tx-fault.py'),str(self.log),'--sq-entries','256']
        r=subprocess.run(cmd,capture_output=True,text=True,timeout=10)
        self.assertEqual(r.returncode,0);self.assertTrue(json.loads(r.stdout)['complete'])
        self.log.write_text(self.text.splitlines()[0]+'\n')
        r=subprocess.run(cmd,capture_output=True,text=True,timeout=10)
        self.assertEqual(r.returncode,1);self.assertFalse(json.loads(r.stdout)['complete'])
        self.log.write_text(self.text)


if __name__ == '__main__':
    unittest.main()
