"""Incomplete periodic flushes and guest stops must not become timing evidence."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('bench_report', ROOT / 'tools/bench_report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class FrameWindowTests(unittest.TestCase):
    def fixture(self, name, retained_seconds, stop=False, dropped=0):
        path = ROOT / 'work/bench-report-tests' / name
        (path / 'captures').mkdir(parents=True, exist_ok=True)
        (path / 'profile').mkdir(exist_ok=True)
        (path / 'captures/present_002048.bmp').write_bytes(b'synthetic scene marker')
        (path / 'startup.log').write_text('present_calls=2048 draw_calls=100\n' +
                                         ('AC1 guest stop: synthetic stop\n' if stop else ''))
        frames = list(range(2047)) + [2047 + i * 40 for i in range(int(retained_seconds * 25) + 1)]
        (path / 'profile/raw_0.json').write_text(json.dumps({
            'mode': 'frames', 'frequency': 1000, 'start': 0, 'frames': frames, 'frame_dropped': dropped}))
        count = int(retained_seconds * 25) + 1
        rows = ['present_index,wall_ms,main_thread_cpu_ms,process_cpu_ms']
        rows.extend(f'{2048 + i},40,5,10' for i in range(1, count + 1))
        (path / 'frame-stats.csv').write_text('\n'.join(rows) + '\n')
        return report.internal_run(path, 60)

    def test_periodic_flush_truncation_is_a_failure(self):
        result = self.fixture('truncated', 53.8)
        self.assertFalse(result['reached_gameplay'])
        self.assertFalse(result['full_frame_window'])
        self.assertIn('incomplete 60-second', result['failure']['reason'])

    def test_post_deadline_flush_preserves_exact_measurement_window(self):
        result = self.fixture('complete', 72)
        self.assertTrue(result['reached_gameplay'])
        self.assertTrue(result['full_frame_window'])
        self.assertEqual(result['measurement_elapsed_seconds'], 60)
        self.assertEqual(result['fps'], 25)
        self.assertEqual(result['frames_in_window'], 1500)
        self.assertAlmostEqual(result['main_thread_cpu_ms_per_frame'], 5)
        self.assertAlmostEqual(result['process_cpu_ms_per_frame'], 10)

    def test_guest_stop_and_lost_frames_are_failures(self):
        self.assertFalse(self.fixture('stopped', 72, stop=True)['reached_gameplay'])
        self.assertFalse(self.fixture('dropped', 72, dropped=1)['reached_gameplay'])

    def test_menu_workload_is_successful_without_claiming_gameplay(self):
        self.fixture('menu', 72)
        result = report.internal_run(ROOT / 'work/bench-report-tests/menu', 60, 'menu')
        self.assertTrue(result['reached_workload'])
        self.assertFalse(result['reached_gameplay'])

    def test_paired_cpu_metric_detects_a_controlled_increase(self):
        def side(cpu):
            return {'label': str(cpu), 'failures': 0, 'runs': [
                {'reached_workload': True, 'main_thread_cpu_ms_per_frame': cpu}]}
        text = report.comparison_text({'pair_count': 1, 'workload': 'gameplay',
            'A': side(5.0), 'B': side(7.0), 'paired_deltas': {}, 'vsync': 'off'})
        self.assertIn('main CPU median ms/frame: 2.0', text)


if __name__ == '__main__':
    unittest.main()
