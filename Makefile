# The sandbox replica of the deployment target and its checks. The C++ build goes through the
# CMake presets (README.md).
.PHONY: e2e-up e2e-down e2e-test validate-manifests

e2e-up:
	deploy/local/e2e-up.sh

e2e-down:
	deploy/local/e2e-down.sh

e2e-test:
	tests/cluster/vod_flow.py

validate-manifests:
	deploy/local/validate-manifests.sh --server
