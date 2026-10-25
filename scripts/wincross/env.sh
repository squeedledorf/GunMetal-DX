# Environment for the Windows cross build (source from the repo root).
# Python.cmake takes the interpreter from $PYTHON (its Windows branch would otherwise
# search the registry); the venv carries llsd for viewer_manifest.py.
export PYTHON=$PWD/.venv/bin/python PATH=$PWD/.venv/bin:$PATH
