"""Short runtime windows and adaptive pair decisions are reported correctly."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('bench_report', ROOT / 'tools/bench_report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class FrameWindowTests(unittest.TestCase):
    def fixture(self, name, retained_frames, stop=False):
        path = ROOT / 'work/bench-report-tests' / name
        (path / 'captures').mkdir(parents=True, exist_ok=True)
        (path / 'startup.log').write_text('AC1 guest stop: synthetic stop\n' if stop else '')
        count = int(retained_frames)
        rows = ['present_index,wall_ms,main_thread_cpu_ms,process_cpu_ms,cpu_sample_window_frames,main_thread_cycles,processor,guest_time_ns,input_command_index']
        rows.extend(f'{1800 + i},40,5,10,16,{1000+i},2,{1000000000+i*40000000},7' for i in range(count))
        (path / 'frame-stats.csv').write_text('\n'.join(rows) + '\n')
        return report.internal_run(path, 0, 'gameplay', 1800, 1800, 1, 2200)

    def test_short_runtime_frame_window_is_a_failure(self):
        result = self.fixture('truncated', 40)
        self.assertFalse(result['reached_gameplay'])
        self.assertFalse(result['full_frame_window'])
        self.assertIn('incomplete Present range', result['failure']['reason'])

    def test_short_periodic_flush_tolerance_keeps_a_complete_window(self):
        result = self.fixture('complete', 401)
        self.assertTrue(result['reached_gameplay'])
        self.assertTrue(result['full_frame_window'])
        self.assertEqual(result['frames_in_window'], 401)
        self.assertEqual(result['fps'], 25)
        self.assertAlmostEqual(result['main_thread_cpu_ms_per_frame'], 5)
        self.assertAlmostEqual(result['process_cpu_ms_per_frame'], 10)
        self.assertAlmostEqual(result['main_thread_cycles_p95'], 1380)
        self.assertEqual(result['input_command_index_start'], result['input_command_index_end'])
        self.assertEqual(result['processor_migrations'], 0)

    def test_multimodal_frame_cost_uses_the_window_mean(self):
        # Short and long frames alternate; a median picks a mode, the mean is the work.
        def run(name, cycles):
            path = ROOT / 'work/bench-report-tests' / name
            (path / 'captures').mkdir(parents=True, exist_ok=True)
            (path / 'startup.log').write_text('')
            rows = ['present_index,wall_ms,main_thread_cpu_ms,process_cpu_ms,cpu_sample_window_frames,'
                    'main_thread_cycles,processor,guest_time_ns,input_command_index']
            rows.extend(f'{1800 + i},{c / 4},5,10,16,{c},2,{1000000000 + i * 40000000},7' for i, c in enumerate(cycles))
            (path / 'frame-stats.csv').write_text('\n'.join(rows) + '\n')
            return report.internal_run(path, 0, 'gameplay', 1800, 1800, 1, 2200)
        a = run('modes-a', ([100] * 3 + [200] * 2) * 80 + [100])
        b = run('modes-b', ([80] * 2 + [150] * 3) * 80 + [80])
        self.assertLess(a['main_thread_cycles_p50'], b['main_thread_cycles_p50'])
        self.assertLess(b['main_thread_cycles_per_frame'], a['main_thread_cycles_per_frame'])
        self.assertLess(b['frametime_mean_ms'], a['frametime_mean_ms'])

    def test_guest_stop_is_a_failure(self):
        self.assertFalse(self.fixture('stopped', 401, stop=True)['reached_gameplay'])

    def test_menu_workload_is_successful_without_claiming_gameplay(self):
        self.fixture('menu', 401)
        result = report.internal_run(ROOT / 'work/bench-report-tests/menu', 0, 'menu', 1800, 1800, 1, 2200)
        self.assertTrue(result['reached_workload'])
        self.assertFalse(result['reached_gameplay'])

    def test_paired_cpu_metric_detects_a_controlled_increase(self):
        def side(cpu):
            return {'label': str(cpu), 'failures': 0, 'runs': [
                {'reached_workload': True, 'main_thread_cpu_ms_per_frame': cpu}]}
        text = report.comparison_text({'pair_count': 1, 'workload': 'gameplay',
            'A': side(5.0), 'B': side(7.0), 'paired_deltas': {}, 'vsync': 'off'})
        self.assertIn('main CPU mean ms/frame: 2.0', text)

    def test_non_equivalent_workload_pair_is_excluded_but_cost_delta_is_reported(self):
        def side(values):
            return {'label': 'same', 'failures': 0, 'runs': [
                {'reached_workload': True, 'main_thread_cpu_ms_per_frame': v,
                 'main_thread_cycles_per_frame': v * 100,
                 'draw_calls_in_window': v * 10} for v in values]}
        data = {'pair_count': 2, 'workload': 'gameplay', 'A': side([100, 1000]),
                'B': side([102, 2000]), 'paired_deltas': {}, 'vsync': 'off',
                'pair_equivalence': [{'pair': 1, 'equivalent': True},
                                     {'pair': 2, 'equivalent': False}]}
        text = report.comparison_text(data)
        self.assertIn('main CPU mean ms/frame: 2 (workload-equivalent pairs=1)', text)
        self.assertIn('draw calls in window: 20 (workload-equivalent pairs=1)', text)

    def test_adaptive_decision_stops_for_a_clear_difference(self):
        result = report.adaptive_decision([9.5, 10.0, 10.5], [250, 252, 251], 3)
        self.assertTrue(result['stop'])
        self.assertEqual(result['verdict'], 'B regression')
        self.assertGreater(result['ci95_low'], 0)

    def test_adaptive_decision_stops_as_neutral_with_a_narrow_ci(self):
        result = report.adaptive_decision([0.0, 0.0, 0.0], [250, 250, 250], 3)
        self.assertTrue(result['stop'])
        self.assertEqual(result['verdict'], 'no meaningful difference')

    def test_adaptive_decision_runs_to_maximum_when_inconclusive(self):
        result = report.adaptive_decision([-3.0, 0.0, 3.0], [250, 250, 250], 8)
        self.assertTrue(result['stop'])
        self.assertEqual(result['executed_pairs'], 8)
        self.assertEqual(result['stop_reason'], 'maximum pairs reached')


if __name__ == '__main__':
    unittest.main()
