# COPY. The canonical file is tools/delv_compat.py in ratlizard/cythera-workbench;
# fix it there and re-copy. This repository is retired, so this copy exists
# only so port/ can run standalone -- do not edit it to diverge.
# Verify with tools/check_copies.sh. Source sha256 13e6caef0215704b189dad01623e8dbd4c5d303ffd617e0a9afc973b7450e179.
"""Make the delvmod package importable on modern Python.

delvmod is a 2014-era Python 2 library. Two things stop it importing on a
modern Python, and both are handled here, from *outside* the checkout --
nothing in delvmod is ever edited, because it is the correctness
oracle and an oracle you have patched is not one.

  * **parsley**, a PEG parser that is not installed. delv/rdasm.py imports it
    at module scope (rdasm.py:900) but only touches it inside
    Assembler.__init__, and delv/__init__.py pulls rdasm in -- so importing
    anything at all fails without it. Reading and disassembling archives never
    builds an assembler, so a stub that defers the failure to the point of use
    keeps the rest of delv working. **This one is still required.**
  * **inspect.getargspec**, removed in 3.11, which rdasm.py used at import time
    to derive assembler operand rules. The checkout this repository points at
    -- ratlizard/delvmod, the maintainer's fork -- fixes this internally at rdasm.py:65, so
    the shim below is dead code against it and is kept only so that $DELVMOD
    can point at an unpatched upstream checkout and still work.

If that ever stops being true and delvmod imports clean, delete this file and
the one import in utilities/delv_graphics_ref.py. It exists to be unnecessary.

Two ways to use it. Import it for its side effects and then use delv normally:

    import tools.delv_compat            # or: import delv_compat
    import delv.archive

or, when the caller resolves the delvmod path itself (as
utilities/delv_graphics_ref.py does, from its command line), take just the
shims:

    from tools.delv_compat import install_shims
    sys.path.insert(0, delv_path)
    install_shims()

Set $DELVMOD to use a delvmod checkout kept outside this repository.
"""
import collections
import inspect
import os
import sys
import types

# getargspec's four fields, under its own names -- delvmod reads .args off the
# result in places, so a plain tuple is not a faithful enough stand-in.
_ArgSpec = collections.namedtuple('ArgSpec', 'args varargs keywords defaults')


def install_shims():
    """Install both compatibility shims. Safe to call more than once."""
    if not hasattr(inspect, 'getargspec'):
        def getargspec(func):
            fas = inspect.getfullargspec(func)
            return _ArgSpec(fas.args, fas.varargs, fas.varkw, fas.defaults)
        inspect.getargspec = getargspec

    if 'parsley' not in sys.modules:
        stub = types.ModuleType('parsley')

        def _unavailable(*a, **kw):
            raise RuntimeError(
                'delv.rdasm.Assembler needs the "parsley" package, which is '
                'not installed; reading and disassembling archives still work.')

        stub.makeGrammar = _unavailable
        stub.__doc__ = 'Stub installed by tools/delv_compat.py.'
        sys.modules['parsley'] = stub


def delvmod_path():
    """Where the delvmod checkout is: $DELVMOD, else the submodule."""
    env = os.environ.get('DELVMOD')
    if env:
        return env
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(root, 'reference', 'delvmod')


# Importing this module is enough to make `import delv.archive` work.
_PKG = delvmod_path()
if os.path.isdir(_PKG) and _PKG not in sys.path:
    sys.path.insert(0, _PKG)
install_shims()
