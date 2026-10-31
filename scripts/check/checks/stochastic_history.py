"""Regression guard for lighting history coordinates, clocks, packing and RR routing."""
import importlib.util
import io
import unittest

NAME = 'stochastic-history'
STAGE = 'source'
DESCRIPTION = 'half-resolution sample centres and cadence-independent lighting histories'
GUARDS = 'Quarter-texel history drift, frame-rate dependent lighting lag, age quantization and metadata format mismatches.'

def run(ctx, result):
    spec = importlib.util.spec_from_file_location('stochastic_history_fixtures',ctx.path('scripts/check/test_stochastic_history.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    output = io.StringIO()
    checked = unittest.TextTestRunner(stream=output).run(unittest.defaultTestLoader.loadTestsFromModule(mod))
    if not checked.wasSuccessful():
        result.fail(output.getvalue())
    result.summary = f'{checked.testsRun} source/math fixtures'
