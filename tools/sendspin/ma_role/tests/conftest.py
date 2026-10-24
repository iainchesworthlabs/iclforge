import sys
from pathlib import Path

# The package is imported as `ma_role` from tools/sendspin, whose scripts sit beside it.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
