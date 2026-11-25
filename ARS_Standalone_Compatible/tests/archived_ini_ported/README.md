# Historical compatibility tests (INI based)

The files in this folder were carried over unmodified from the older runtime-INI server architecture. They reference `--config`, temporary `server.ini` fixtures, and source patterns that **no longer exist in the compiled-settings build**. They are intentionally archived, not counted as passing, and must be rewritten to use the isolated `ARS_LAB_PROFILE` and/or new admin script interfaces before they can form part of the acceptance suite. The original reference tests also remain under `tests/reference_original`.
