#!/usr/bin/env python3
"""Unknown imports must fail even when Dependencies: OK was printed."""
from pathlib import Path
import subprocess

tool = Path(__file__).with_name('check-unresolved-symbols.py')
reserved = ['__ZN19IONetworkController29_RESERVEDIONetworkController%dEv' % n
            for n in [2, 3, 4, 5]]
fixtures = [([], 0), (reserved, 0), (reserved + ['_thread_call_isactive'], 1),
            (['_thread_call_cancel_wait'], 1), (['_unknown'], 1),
            ([reserved[0], reserved[0]], 0),
            (['__ZN19IONetworkController29_RESERVEDIONetworkController6Ev'], 1)]
for symbols, expected in fixtures:
    run = subprocess.run(['/usr/bin/python3', str(tool)], input='\n'.join(symbols),
                         text=True, capture_output=True, check=False)
    assert run.returncode == expected, (symbols, run.returncode, run.stdout)
    if expected:
        assert 'FAIL: unresolvable non-reserved symbol:' in run.stdout
print('symbol guard: 7 cases passed (actual checker subprocess)')
