# The sandbox replica of the deployment target and its checks. The C++ build goes through the
# CMake presets (README.md).
.PHONY: e2e-up e2e-down e2e-test validate-manifests

e2e-up:
	deploy/local/e2e-up.sh

e2e-down:
	deploy/local/e2e-down.sh

# Always the sandbox: a ULW_E2E_URL left in the environment would aim the run at a real
# deployment, which only an explicit, named invocation may do (deploy/askedin/RUNBOOK.md).
e2e-test:
	python3 tests/cluster/guard_test.py
	env -u ULW_E2E_URL -u ULW_E2E_TOKEN tests/cluster/vod_flow.py

validate-manifests:
	deploy/local/validate-manifests.sh --server
