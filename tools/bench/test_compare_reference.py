import contextlib
import io
import pathlib
import sys
import tempfile
import unittest
try:
    import numpy as np
    from PIL import Image
except ImportError as e:
    print(f"skipped: {e}"); sys.exit(77)
import compare_reference as cr
class CompareReferenceTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.ref=pathlib.Path(self.tmp.name)/"ref"; self.new=pathlib.Path(self.tmp.name)/"new"
        self.base=np.zeros((8,16,3),dtype=np.uint8); self.base[...,0]=np.arange(16,dtype=np.uint8)*16
        for name in ("a.bmp","sub/b.bmp"): self.write(self.ref/name,self.base); self.write(self.new/name,self.base)
    def write(self,p,a):
        p.parent.mkdir(parents=True,exist_ok=True); Image.fromarray(a,"RGB").save(p,"BMP")
    def run_cli(self,*args):
        out=io.StringIO()
        with contextlib.redirect_stdout(out): code=cr.main([str(x) for x in args])
        return code,out.getvalue()
    def change(self):
        a=self.base.copy(); a[3,5,1]+=30; self.write(self.new/"a.bmp",a)
    def test_identical(self):
        code,text=self.run_cli(self.ref,self.new); self.assertEqual(code,0); self.assertEqual(text.count("identical"),2)
    def test_difference(self):
        self.change(); code,text=self.run_cli(self.ref,self.new); self.assertEqual(code,1); self.assertIn("MAD 0.0781",text)
    def test_tolerance(self):
        self.change(); self.assertEqual(self.run_cli(self.ref,self.new,"--mad-limit","0.1")[0],0)
    def test_missing(self):
        (self.new/"sub/b.bmp").unlink(); self.assertEqual(self.run_cli(self.ref,self.new)[0],2)
    def test_size(self):
        self.write(self.new/"a.bmp",self.base[:4]); self.assertEqual(self.run_cli(self.ref,self.new)[0],2)
    def test_pair(self):
        self.change(); self.assertEqual(self.run_cli("--pair",self.ref/"a.bmp",self.new/"a.bmp")[0],1)
        self.assertEqual(self.run_cli("--pair",self.ref/"a.bmp",self.ref/"sub/b.bmp")[0],0)
if __name__=="__main__": unittest.main()
