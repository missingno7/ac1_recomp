"""Controlled replay input archives previous saves and refuses outside paths."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('bench_userdata',ROOT/'tools/bench_userdata.py')
bench=importlib.util.module_from_spec(spec);spec.loader.exec_module(bench)


class UserdataSeedTests(unittest.TestCase):
    def test_restore_archives_prior_state_and_hashes_exact_seed(self):
        (ROOT/'work').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT/'work') as tmp:
            root=Path(tmp);work=root/'work';work.mkdir()
            seed=work/'seed';seed.mkdir();(seed/'profile').write_bytes(b'input')
            live=work/'userdata';live.mkdir();(live/'previous').write_bytes(b'keep')
            archive=work/'evidence/prior'
            manifest=bench.restore(seed,archive,root)
            self.assertEqual((archive/'previous').read_bytes(),b'keep')
            self.assertEqual((live/'profile').read_bytes(),b'input')
            self.assertFalse((live/'previous').exists())
            self.assertEqual(manifest[0]['bytes'],5)
            self.assertEqual(manifest[0]['path'],'profile')
            (live/'profile').write_bytes(b'changed')
            again=bench.restore(seed,work/'evidence/prior2',root)
            self.assertEqual(manifest,again)

    def test_refuse_nonprivate_seed_or_existing_archive_before_mutation(self):
        (ROOT/'work').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT/'work') as tmp:
            root=Path(tmp);work=root/'work';work.mkdir()
            seed=work/'seed';seed.mkdir();live=work/'userdata';live.mkdir()
            (live/'keep').write_bytes(b'unchanged')
            archive=work/'existing';archive.mkdir()
            for source,destination in ((seed,archive),(root,work/'new'),(seed,root/'outside'),(live,work/'new'),(seed,seed/'archive')):
                with self.assertRaises(ValueError):bench.restore(source,destination,root)
                self.assertEqual((live/'keep').read_bytes(),b'unchanged')
            original=bench.linked
            with patch.object(bench,'linked',side_effect=lambda p:p==seed or original(p)):
                with self.assertRaisesRegex(ValueError,'linked'):bench.restore(seed,work/'new',root)
            self.assertEqual((live/'keep').read_bytes(),b'unchanged')


if __name__ == '__main__':unittest.main()
