# The sandbox replica of the deployment target and its checks. The C++ build goes through the
# CMake presets (README.md).
.PHONY: e2e-up e2e-down e2e-test e2e-load e2e-stunner validate-manifests

e2e-up:
	deploy/local/e2e-up.sh

e2e-down:
	deploy/local/e2e-down.sh

# Always the sandbox: a ULW_E2E_URL left in the environment would aim the run at a real
# deployment, which only an explicit, named invocation may do (deploy/askedin/RUNBOOK.md).
e2e-test:
	python3 tests/cluster/guard_test.py
	env -u ULW_E2E_URL -u ULW_E2E_TOKEN tests/cluster/vod_flow.py

# 500 concurrent uploads through the route while kubectl top is sampled (M14); ULW_LOAD_UPLOADS
# lowers the count on a small machine. The reports land in load-report/.
e2e-load:
	python3 tests/cluster/load_check_test.py
	deploy/local/metrics-server.sh
	env -u ULW_E2E_URL -u ULW_E2E_TOKEN tests/cluster/load_check.py --uploads $(or $(ULW_LOAD_UPLOADS),500)

# STUNner and LiveKit (M26), on their own or into the cluster e2e-up made, then checked from a
# client outside the cluster's network.
e2e-stunner:
	python3 tests/cluster/stun_test.py
	deploy/stunner/up.sh
	tests/cluster/stunner_check.py

validate-manifests:
	deploy/local/validate-manifests.sh --server
