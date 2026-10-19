import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("resume_analysis", Path(__file__).resolve().parents[1]/"analyze_resume_baseline.py")
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class AnalysisTests(unittest.TestCase):
    def fixture(self, root, scope="process", malformed=False, unknown=2, loss=False):
        fields = ["event","phase","window","dirty_mib","round","scope","pids","valid","taskclk_ms"]
        rows = [dict(zip(fields,[i,"stat","return",256,i,scope,"1;2;3",1,20])) for i in (1,2)]
        for i in (3,4):
            rows.append(dict(zip(fields,[i,"record","return",256,i,scope,"1;2;3",1,23])))
            d=root/f"{i:04d}_record_return_256m"; d.mkdir()
            text=(f" 30.00% 30 [kernel.kallsyms] [k] _copy_to_iter\n"
                  f" 20.00% 20 [kernel.kallsyms] [k] filemap_get_pages\n"
                  f"  2.00% {unknown} [unknown] [.] 0xffff123456789abc\n"
                  f" 48.00% {50-unknown} server [.] unknown_business_function\n")
            if malformed: text+=" 1.00% broken row\n"
            (d/"symbols.txt").write_text(text)
            (d/"perf.stderr").write_text("lost 5 samples" if loss else "")
        with (root/"events.csv").open("w",newline="") as f:
            writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(rows)

    def test_conservation_categories_and_matching_baseline(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);self.fixture(root)
            result=analysis.analyze(root,min_events=2,min_samples=100)[0]
            self.assertTrue(result["eligible_for_conditional_inference"])
            self.assertEqual(result["baseline_mean_cpu_ms"],20)
            self.assertAlmostEqual(result["categories"]["copy"]["sample_share"],.30)
            self.assertAlmostEqual(sum(x["sample_share"] for x in result["categories"].values()),1)
            self.assertEqual(result["categories"]["copy"]["cluster95"],[.3,.3])
            self.assertEqual(analysis.category("server","runtime.memmove"),"copy")

    def test_quality_gates_and_unknown_preserved(self):
        for params,reason in [({"scope":"system"},"system_scope"),
                              ({"malformed":True},"report_parse_or_loss"),
                              ({"unknown":20},"unresolved_symbols"),
                              ({"loss":True},"report_parse_or_loss")]:
            with self.subTest(params=params),tempfile.TemporaryDirectory() as t:
                root=Path(t);self.fixture(root,**params)
                result=analysis.analyze(root,min_events=2,min_samples=100)[0]
                self.assertFalse(result["eligible_for_conditional_inference"])
                self.assertIn(reason,result["gate_failures"])

    def test_empty_is_not_zero_copy(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);(root/"events.csv").write_text("event,phase,valid\n")
            self.assertEqual(analysis.analyze(root),[])
            self.assertIn("不得解释为copy占比为零",(root/"composition.md").read_text(encoding="utf-8"))


if __name__=="__main__":
    unittest.main()
