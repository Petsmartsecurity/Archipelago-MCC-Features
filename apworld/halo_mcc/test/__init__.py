import os

# Halo 2 is experimental (generate_early raises OptionError without it); test_release.py clears it where it matters
os.environ.setdefault("HALOAP_EXPERIMENTAL", "1")
