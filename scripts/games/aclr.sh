# Armored Core: Last Raven Portable -- the title this project was built on.
#
# The disc the pipeline was measured against. Phase 0 ran on the US PSN SKU,
# not the UMD retail SKU (ULUS-10493) -- different EBOOT, re-signed with a
# 6.xx-era key. 00-identify.sh warns if yours differs; addresses are per-build.
MODULE="ACLR_App"               # the ~PSP module name; names the decrypted ELF
PREFIX="aclr"                   # emit prefix: <prefix>_funcs.c, <prefix>_imports.c
EXPECT_DISC_ID="NPUH10024"
TITLE="Armored Core: Last Raven"
