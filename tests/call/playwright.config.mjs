import { defineConfig } from '@playwright/test';

// run.sh sets ULW_E2E_CHROME to the pinned Chrome for Testing build tests/e2e uses.
export default defineConfig({
  testDir: '.',
  testMatch: '*.spec.mjs',
  // One LiveKit server and one room per test.
  workers: 1,
  timeout: 180_000,
  reporter: [['list']],
});
