#!/bin/sh
python3 -m unittest -v \
  tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_code_only_trusted_config_directories_install \
  tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_code_only_untrusted_config_directory_rejected_before_intent \
  tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_ota_owner_untrusted_config_directory_rejected_before_mutation_barrier \
  tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_ota_owner_trusted_config_directories_complete \
  > evidence/20261003-config-publication/directory-complete.stdout.raw \
  2> evidence/20261003-config-publication/directory-complete.stderr.raw
status=$?
printf '%s\n' "$status" > evidence/20261003-config-publication/directory-complete.exit.txt
exit "$status"
